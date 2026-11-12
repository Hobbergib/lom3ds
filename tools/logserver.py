#!/usr/bin/env python3
"""Collects log files sent by the port's start screen (X: Send logs).

Usage: logserver.py [OUT_DIR]

Answers the console's broadcast so it can find this computer, then stores
each file it sends as OUT_DIR/<time>-<name>. Listens on port 28771 (UDP and
TCP); a firewall has to let the local network reach it. Runs until stopped.
"""
import pathlib
import re
import socket
import sys
import threading
import time

PORT = 28771
MAX_SIZE = 16 << 20


def answer_discovery():
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp.bind(("", PORT))
    while True:
        data, sender = udp.recvfrom(64)
        if data == b"LOM3DS-LOG-DISCOVER":
            udp.sendto(b"LOM3DS-LOG-HERE", sender)


def receive(conn, out):
    stream = conn.makefile("rb")
    stamp = time.strftime("%Y%m%d-%H%M%S")
    while True:
        m = re.fullmatch(rb"LOM3DS-LOG ([a-z.]+) (\d+)\n", stream.readline(128))
        if not m or int(m.group(2)) > MAX_SIZE:
            return
        name, size = m.group(1).decode(), int(m.group(2))
        data = stream.read(size)
        path = out / f"{stamp}-{name}"
        path.write_bytes(data)
        print(f"received {path} ({len(data)} bytes)", flush=True)


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "logs")
    out.mkdir(parents=True, exist_ok=True)
    threading.Thread(target=answer_discovery, daemon=True).start()
    tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp.bind(("", PORT))
    tcp.listen(4)
    print(f"waiting for logs on port {PORT}; they go to {out.resolve()}", flush=True)
    while True:
        conn, sender = tcp.accept()
        with conn:
            conn.settimeout(20)
            try:
                receive(conn, out)
            except OSError as e:
                print(f"transfer from {sender[0]} failed: {e}", flush=True)


if __name__ == "__main__":
    main()
