#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""LAN throughput peer for `sockbench` — TCP and UDP (stdlib only).

    python3 tcp-bench-peer.py [--sink-rate KBPS] [--sink-stall ON:OFF] [bind-address]

Port 5001: sink   — discards whatever the client sends (sockbench tx / udptx)
Port 5002: source — blasts data at the client           (sockbench rx / udprx)

Both ports listen on TCP and UDP simultaneously. The Amiga side measures
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

UDP has no connection, so flows are keyed by peer address and delimited by
one-byte "guns": the client sends "G<datagram-bytes>" to a source to start it
(and learn where/what size to blast), and "S" to a sink/source to stop it and
trigger its report. A lost stop gun is covered by an idle/duration timeout.
"""

import socket
import sys
import threading
import time

SINK_PORT = 5001
SOURCE_PORT = 5002
CHUNK = 256 * 1024

UDP_DEFAULT_DGRAM = 1472  # one 1500-MTU frame, if a start gun omits the size
UDP_IDLE_TIMEOUT = 3.0    # report + drop a silent UDP flow (lost stop gun)
UDP_MAX_BLAST = 60.0      # safety cap on how long a source blasts one client

SINK_RATE = 0.0           # KB/s the TCP sink will read; 0 = as fast as possible
SINK_STALL_ON = 0.0       # seconds the TCP sink stops reading entirely
SINK_STALL_OFF = 0.0      # seconds it reads between stalls


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


def udp_report(role, addr, st):
    report(role, f"{addr[0]}:{addr[1]}", st[0], st[2] - st[1])


def udp_sink(port, bind_addr):
    """Count datagrams per source address (sockbench udptx)."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((bind_addr, port))
    srv.settimeout(1.0)
    log(f"udp listening on {bind_addr or '*'}:{port} (sink)")
    flows = {}  # addr -> [bytes, start_ts, last_ts]
    while True:
        try:
            data, addr = srv.recvfrom(65535)
        except socket.timeout:
            data = None
        now = time.monotonic()
        if data == b"S":  # stop gun
            st = flows.pop(addr, None)
            if st:
                udp_report("sink  ", addr, st)
        elif data is not None:
            st = flows.get(addr)
            if st is None:
                st = flows[addr] = [0, now, now]
                log(f"udp-sink flow from {addr[0]}:{addr[1]}")
            st[0] += len(data)
            st[2] = now
        for addr in [a for a, s in flows.items() if now - s[2] > UDP_IDLE_TIMEOUT]:
            udp_report("sink  ", addr, flows.pop(addr))


def udp_source(port, bind_addr):
    """Blast datagrams to each client that fired a start gun (sockbench udprx)."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((bind_addr, port))
    srv.setblocking(False)
    log(f"udp listening on {bind_addr or '*'}:{port} (source)")
    blob = b"x" * 65507
    clients = {}  # addr -> [bytes, start_ts, last_ts, dgram_size]
    while True:
        while True:  # drain control datagrams (start/stop guns)
            try:
                data, addr = srv.recvfrom(65535)
            except (BlockingIOError, OSError):
                break
            now = time.monotonic()
            if data.startswith(b"G"):
                try:
                    dgram = int(data[1:]) if len(data) > 1 else UDP_DEFAULT_DGRAM
                except ValueError:
                    dgram = UDP_DEFAULT_DGRAM
                dgram = max(1, min(dgram, len(blob)))
                clients[addr] = [0, now, now, dgram]
                log(f"udp-source blast to {addr[0]}:{addr[1]} ({dgram} B datagrams)")
            elif data == b"S":
                st = clients.pop(addr, None)
                if st:
                    udp_report("source", addr, st)
        if not clients:
            time.sleep(0.02)
            continue
        now = time.monotonic()
        for addr in list(clients):
            st = clients[addr]
            try:
                srv.sendto(blob[: st[3]], addr)
                st[0] += st[3]
                st[2] = now
            except (BlockingIOError, OSError):
                pass
            if now - st[1] > UDP_MAX_BLAST:  # lost stop gun
                udp_report("source", addr, clients.pop(addr))


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
