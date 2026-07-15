#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""LAN throughput peer for `sockbench` (stdlib only).

    python3 tcp-bench-peer.py [bind-address]

Port 5001: sink   — discards whatever the client sends (sockbench tx)
Port 5002: source — blasts data until the client closes  (sockbench rx)

The Amiga side measures itself; this peer just moves bytes, but prints its
own per-connection byte count and rate as a cross-check.
"""

import socket
import sys
import threading
import time

SINK_PORT = 5001
SOURCE_PORT = 5002
CHUNK = 256 * 1024


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
    threading.Thread(target=serve, args=(SINK_PORT, sink, bind_addr), daemon=True).start()
    serve(SOURCE_PORT, source, bind_addr)


if __name__ == "__main__":
    main()
