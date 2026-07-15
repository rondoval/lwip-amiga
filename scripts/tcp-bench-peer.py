#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""LAN throughput peer for `sockbench` — TCP and UDP (stdlib only).

    python3 tcp-bench-peer.py [bind-address]

Port 5001: sink   — discards whatever the client sends (sockbench tx / udptx)
Port 5002: source — blasts data at the client           (sockbench rx / udprx)

Both ports listen on TCP and UDP simultaneously. The Amiga side measures
itself; this peer just moves bytes, but prints its own per-flow byte count
and rate as a cross-check (for UDP that difference is the loss).

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


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def report(role, peer, nbytes, secs):
    mbps = nbytes * 8 / secs / 1e6 if secs > 0 else 0.0
    log(f"{role} {peer}: {nbytes} bytes in {secs:.2f} s = {mbps:.1f} Mb/s")


def sink(conn, peer):
    total, start = 0, time.monotonic()
    try:
        while True:
            data = conn.recv(CHUNK)
            if not data:
                break
            total += len(data)
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
    bind_addr = sys.argv[1] if len(sys.argv) > 1 else ""
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
