#!/usr/bin/env python3
"""One-shot TCP echo/integrity test; run inside the isolated PPP test namespaces."""
import argparse
import hashlib
import socket
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("role", choices=["listen", "connect"])
p.add_argument("address")
p.add_argument("--port", type=int, default=28800)
p.add_argument("--bytes", type=int, default=32768)
args = p.parse_args()
with socket.socket() as s:
    s.settimeout(180)
    if args.role == "listen":
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((args.address, args.port))
        s.listen(1)
        print("READY", flush=True)
        conn, _ = s.accept()
        with conn:
            conn.settimeout(180)
            while data := conn.recv(4096):
                conn.sendall(data)
    else:
        s.connect((args.address, args.port))
        payload = hashlib.shake_256(b"Zigmodem PPP v4 integrity").digest(args.bytes)
        started = time.monotonic()
        s.sendall(payload)
        received = bytearray()
        while len(received) < len(payload):
            data = s.recv(min(4096, len(payload)-len(received)))
            if not data:
                raise RuntimeError("Premature TCP EOF")
            received.extend(data)
        if received != payload:
            raise RuntimeError("TCP echo content mismatch")
        print(f"PASS {len(payload)} bytes each direction, SHA256 "
              f"{hashlib.sha256(received).hexdigest()}, {time.monotonic()-started:.2f}s")
