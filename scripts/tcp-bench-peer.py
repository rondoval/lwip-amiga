#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""LAN throughput and latency peer for `sockbench` — TCP and UDP (stdlib only).

    python3 tcp-bench-peer.py [--sink-rate KBPS] [--sink-stall ON:OFF] [bind-address]

Port 5001: sink   — discards whatever the client sends (sockbench tx / udptx)
Port 5002: source — blasts data at the client           (sockbench rx / udprx)
Port 5003: rr     — answers each request with N bytes    (sockbench rr / rrsel)

5001 and 5002 listen on TCP and UDP simultaneously; 5003 is TCP only. The Amiga side measures
itself; this peer just moves bytes, but prints its own per-flow byte count
and rate as a cross-check (for UDP that difference is the loss).

By default this peer drains as fast as it can, so the Amiga's send window
never closes: `sockbench txblock` then loops through tcp_write without ever
sleeping in sb_send_block, and the blocking send path is only half exercised.
Two knobs make the sink the bottleneck instead:

    --sink-rate 200        read at ~200 KB/s, keeping the window near zero
    --sink-stall 2:5       read 5 s, then stop reading entirely for 2 s

Either one drives tcp_sndbuf() to 0, which is what puts the caller into
sb_send_block -> sb_wait_to (drop ns_Core, sleep, re-acquire) and opens the
mid-write lock break. --sink-stall additionally forces a true zero window and
persist probes. Neither produces retransmissions on a clean LAN; for that, add
loss on this host's interface, e.g.

    sudo tc qdisc add dev eth0 root netem loss 1% delay 20ms
    sudo tc qdisc del dev eth0 root          # to remove

The rr port is a request/response ping-pong: each request starts with an 8-byte
big-endian header {u32 resp_len, u32 req_len}, padded with filler to req_len bytes
in total; the reply is resp_len bytes. The client times every exchange, so this
peer's own turnaround (tens of microseconds of Python) is inside every sample —
constant across an A/B on the Amiga side, which is what the mode is for.

UDP has no connection, so flows are keyed by peer address and delimited by
one-byte "guns": the client sends "G<datagram-bytes>[/<mbps>]" to a source to
start it (and learn where to send, how big to make each datagram and how fast),
and "S" to a sink/source to stop it and trigger its report. A lost stop gun is
covered by an idle/duration timeout.

The source paces itself to the requested rate (iperf's -b): a UDP receive
benchmark is "does the client keep up with a stream of X Mb/s", and the
client's received rate against X is the loss.
"""

import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass

SINK_PORT = 5001
SOURCE_PORT = 5002
RR_PORT = 5003
CHUNK = 256 * 1024

RR_HDR = struct.Struct(">II")            # resp_len, req_len (header included)
RR_BLOB = memoryview(b"x" * (8 * 1024 * 1024))  # largest reply; sockbench clamps to the same

UDP_DEFAULT_DGRAM = 1472  # one 1500-MTU frame, if a start gun omits the size
UDP_IDLE_TIMEOUT = 3.0    # report + drop a silent UDP flow (lost stop gun)
UDP_MAX_BLAST = 60.0      # safety cap on how long a source blasts one client
UDP_PACE_BURST = 64       # datagrams a paced flow may catch up per loop pass

SINK_RATE = 0.0           # KB/s the TCP sink will read; 0 = as fast as possible
SINK_STALL_ON = 0.0       # seconds the TCP sink stops reading entirely
SINK_STALL_OFF = 0.0      # seconds it reads between stalls


@dataclass
class UdpFlow:
    """One UDP flow's byte count and timing, keyed by peer address (`flows`
    in udp_sink, `clients` in udp_source). dgram_size/rate_bps are set only
    by a source flow; rate_bps 0 means unpaced (also true of every sink
    flow, which never sets it)."""
    start_ts: float
    last_ts: float
    bytes: int = 0
    dgram_size: int = 0
    rate_bps: int = 0


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def report(role, peer, nbytes, secs):
    mbps = nbytes * 8 / secs / 1e6 if secs > 0 else 0.0
    log(f"{role} {peer}: {nbytes} bytes in {secs:.2f} s = {mbps:.1f} Mb/s")


def sink(conn, peer):
    total, start = 0, time.monotonic()
    # Read in small pieces when throttled: one 256 KB recv would swallow the
    # whole window in a single call and defeat the rate limit.
    chunk = CHUNK if SINK_RATE <= 0 else max(1024, int(SINK_RATE * 1024 / 20))
    next_stall = start + SINK_STALL_OFF if SINK_STALL_ON > 0 else None
    try:
        while True:
            now = time.monotonic()
            if next_stall is not None and now >= next_stall:
                # Stop reading entirely: the window closes to zero and the
                # sender falls back on persist probes.
                log(f"sink   {peer}: stalling {SINK_STALL_ON:.1f} s")
                time.sleep(SINK_STALL_ON)
                next_stall = time.monotonic() + SINK_STALL_OFF
                continue
            data = conn.recv(chunk)
            if not data:
                break
            total += len(data)
            if SINK_RATE > 0:
                # Sleep off however long these bytes "should" have taken.
                target = start + total / (SINK_RATE * 1024)
                behind = target - time.monotonic()
                if behind > 0:
                    time.sleep(behind)
    except OSError:
        pass
    finally:
        conn.close()
        report("sink  ", peer, total, time.monotonic() - start)


def source(conn, peer):
    total, start = 0, time.monotonic()
    blob = b"x" * CHUNK
    try:
        # Start gun: the client fires one byte once ALL its streams are
        # connected. Blasting on accept starves the later handshakes on
        # the Amiga side (RX pool dries up -> SYN-ACKs dropped -> ECONNABORTED).
        if not conn.recv(1):
            return
        start = time.monotonic()
        while True:
            conn.sendall(blob)
            total += len(blob)
    except OSError:
        pass
    finally:
        conn.close()
        report("source", peer, total, time.monotonic() - start)


def recv_exact(conn, n):
    """n bytes, or None if the peer closed first."""
    buf = bytearray(n)
    view, got = memoryview(buf), 0
    while got < n:
        r = conn.recv_into(view[got:])
        if r == 0:
            return None
        got += r
    return buf


def rr(conn, peer):
    total, trans, start = 0, 0, time.monotonic()
    try:
        while True:
            hdr = recv_exact(conn, RR_HDR.size)
            if hdr is None:
                break
            resp_len, req_len = RR_HDR.unpack(hdr)
            if req_len > RR_HDR.size and recv_exact(conn, req_len - RR_HDR.size) is None:
                break
            reply = RR_BLOB[: min(resp_len, len(RR_BLOB))]
            conn.sendall(reply)
            total += len(reply)
            trans += 1
    except OSError:
        pass
    finally:
        conn.close()
        secs = time.monotonic() - start
        log(f"rr     {peer}: {trans} exchanges in {secs:.2f} s")
        report("rr    ", peer, total, secs)


def udp_report(role, addr, flow):
    peer = f"{addr[0]}:{addr[1]}"
    if flow.rate_bps > 0:
        log(f"{role} {peer}: paced at {flow.rate_bps / 1e6:.0f} Mb/s")
    report(role, peer, flow.bytes, flow.last_ts - flow.start_ts)


def udp_sink(port, bind_addr):
    """Count datagrams per source address (sockbench udptx)."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((bind_addr, port))
    srv.settimeout(1.0)
    log(f"udp listening on {bind_addr or '*'}:{port} (sink)")
    flows = {}  # addr -> UdpFlow
    while True:
        try:
            data, addr = srv.recvfrom(65535)
        except socket.timeout:
            data = None
        now = time.monotonic()
        if data == b"S":  # stop gun
            flow = flows.pop(addr, None)
            if flow:
                udp_report("sink  ", addr, flow)
        elif data is not None:
            flow = flows.get(addr)
            if flow is None:
                flow = flows[addr] = UdpFlow(start_ts=now, last_ts=now)
                log(f"udp-sink flow from {addr[0]}:{addr[1]}")
            flow.bytes += len(data)
            flow.last_ts = now
        for addr in [a for a, f in flows.items() if now - f.last_ts > UDP_IDLE_TIMEOUT]:
            udp_report("sink  ", addr, flows.pop(addr))


def udp_source(port, bind_addr):
    """Stream datagrams to each client that fired a start gun (sockbench udprx):
    paced to the gun's rate, or an unpaced blast when it names none."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((bind_addr, port))
    srv.setblocking(False)
    log(f"udp listening on {bind_addr or '*'}:{port} (source)")
    blob = b"x" * 65507
    clients = {}  # addr -> UdpFlow
    while True:
        while True:  # drain control datagrams (start/stop guns)
            try:
                data, addr = srv.recvfrom(65535)
            except (BlockingIOError, OSError):
                break
            now = time.monotonic()
            if data.startswith(b"G"):
                size_s, _, rate_s = data[1:].decode(errors="replace").partition("/")
                try:
                    dgram = int(size_s) if size_s else UDP_DEFAULT_DGRAM
                except ValueError:
                    dgram = UDP_DEFAULT_DGRAM
                try:
                    rate_bps = int(float(rate_s) * 1e6) if rate_s else 0
                except ValueError:
                    rate_bps = 0
                dgram = max(1, min(dgram, len(blob)))
                clients[addr] = UdpFlow(start_ts=now, last_ts=now, dgram_size=dgram, rate_bps=rate_bps)
                pace = f"paced at {rate_bps / 1e6:.0f} Mb/s" if rate_bps > 0 else "UNPACED flood"
                log(f"udp-source to {addr[0]}:{addr[1]} ({dgram} B datagrams, {pace})")
            elif data == b"S":
                flow = clients.pop(addr, None)
                if flow:
                    udp_report("source", addr, flow)
        if not clients:
            time.sleep(0.02)
            continue
        now = time.monotonic()
        idle = True
        for addr in list(clients):
            flow = clients[addr]
            if flow.rate_bps > 0:
                # token bucket: send what the rate allows by now, in a bounded
                # burst so one flow cannot starve another
                allowed = (now - flow.start_ts) * flow.rate_bps / 8
                burst = 0
                while flow.bytes < allowed and burst < UDP_PACE_BURST:
                    try:
                        srv.sendto(blob[: flow.dgram_size], addr)
                    except (BlockingIOError, OSError):
                        break
                    flow.bytes += flow.dgram_size
                    flow.last_ts = now
                    burst += 1
                if burst:
                    idle = False
            else:
                try:
                    srv.sendto(blob[: flow.dgram_size], addr)
                    flow.bytes += flow.dgram_size
                    flow.last_ts = now
                    idle = False
                except (BlockingIOError, OSError):
                    pass
            if now - flow.start_ts > UDP_MAX_BLAST:  # lost stop gun
                udp_report("source", addr, clients.pop(addr))
        if idle:
            time.sleep(0.001)  # every paced flow is ahead of its clock


def serve(port, handler, bind_addr):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((bind_addr, port))
    srv.listen(16)
    log(f"listening on {bind_addr or '*'}:{port} ({handler.__name__})")
    while True:
        conn, addr = srv.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        peer = f"{addr[0]}:{addr[1]}"
        log(f"{handler.__name__} connection from {peer}")
        threading.Thread(target=handler, args=(conn, peer), daemon=True).start()


def main():
    global SINK_RATE, SINK_STALL_ON, SINK_STALL_OFF

    args = sys.argv[1:]
    positional = []
    while args:
        arg = args.pop(0)
        if arg == "--sink-rate" and args:
            SINK_RATE = float(args.pop(0))
        elif arg == "--sink-stall" and args:
            on, _, off = args.pop(0).partition(":")
            SINK_STALL_ON = float(on)
            SINK_STALL_OFF = float(off) if off else 5.0
        elif arg.startswith("--"):
            print(__doc__)
            return
        else:
            positional.append(arg)
    bind_addr = positional[0] if positional else ""

    if SINK_RATE > 0:
        log(f"TCP sink throttled to {SINK_RATE:.0f} KB/s")
    if SINK_STALL_ON > 0:
        log(f"TCP sink stalls {SINK_STALL_ON:.1f} s every {SINK_STALL_OFF:.1f} s")

    listeners = (
        (serve, (SINK_PORT, sink, bind_addr)),
        (serve, (SOURCE_PORT, source, bind_addr)),
        (serve, (RR_PORT, rr, bind_addr)),
        (udp_sink, (SINK_PORT, bind_addr)),
        (udp_source, (SOURCE_PORT, bind_addr)),
    )
    for fn, fn_args in listeners:
        threading.Thread(target=fn, args=fn_args, daemon=True).start()
    try:
        while True:
            time.sleep(0.25)
    except KeyboardInterrupt:
        log("shutting down")


if __name__ == "__main__":
    main()
