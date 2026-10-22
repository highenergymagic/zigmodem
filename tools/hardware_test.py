#!/usr/bin/env python3
"""Two-host radio integration test; run answer first, then dial on the other host.

SSH carries only test control/output. All test payload crosses the dongle radios.
Uses a paced stream because the stock boards do not expose modem flow control.
"""
import argparse
import hashlib
import time
import serial

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("port")
p.add_argument("role", choices=["answer", "dial"])
p.add_argument("--number", help="Remote AT+PHONUM? value; needed for dial")
p.add_argument("--bytes", type=int, default=4096)
args = p.parse_args()
s = serial.Serial(port=None, baudrate=9600, timeout=0.05, exclusive=True)
s.dtr = s.rts = False
s.port = args.port
s.open()

def wait_for(token, timeout=15):
    data = bytearray()
    framed = b"\r\n" + token + b"\r\n"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        # Never consume stream bytes following CONNECT in the same USB read.
        data.extend(s.read(1))
        if framed in data:
            print(repr(bytes(data)), flush=True)
            return bytes(data)
    raise RuntimeError(f"Timed out waiting for {token!r}: {data!r}")

def command(text, token=b"OK"):
    s.write(text.encode() + b"\r")
    return wait_for(token)

try:
    # Some bridge revisions briefly reset the SoC when the port opens.
    time.sleep(0.5)
    s.reset_input_buffer()
    command("ATE0")
    command("AT+PHONUM?")
    if args.role == "answer":
        command("ATS0=0")
        print("READY", flush=True)
        wait_for(b"RING", 120)
        command("ATA", b"CONNECT")
    else:
        if not args.number:
            raise ValueError("dial requires --number")
        command("ATDT" + args.number, b"CONNECT")
    own = 17 if args.role == "dial" else 29
    other = 29 if args.role == "dial" else 17
    outgoing = bytes((i * own + i // 256) % 256 for i in range(args.bytes))
    expected = bytes((i * other + i // 256) % 256 for i in range(args.bytes))
    incoming = bytearray()
    sent = 0
    started = time.monotonic()
    next_send = started + 0.5
    deadline = started + 90
    while len(incoming) < len(expected) or sent < len(outgoing):
        now = time.monotonic()
        if now > deadline:
            raise RuntimeError(f"Transfer timeout: sent {sent}, received {len(incoming)}; "
                               f"first={incoming[:180]!r}; last={incoming[-100:]!r}")
        if sent < len(outgoing) and now >= next_send:
            n = s.write(outgoing[sent:sent+32])
            sent += n
            next_send = now + 0.02
        incoming.extend(s.read(min(256, max(1, len(expected)-len(incoming)))))
    if bytes(incoming) != expected:
        raise RuntimeError("Binary content mismatch")
    # Each peer proves it received the other's entire stream before hangup.
    # A successful host write alone does not mean the radio queue is drained.
    s.write(b"ZMDONE1!")
    barrier = bytearray()
    deadline = time.monotonic() + 30
    while len(barrier) < 8 and time.monotonic() < deadline:
        barrier.extend(s.read(8-len(barrier)))
    if barrier != b"ZMDONE1!":
        raise RuntimeError(f"Completion handshake failed: {barrier!r}")
    print(f"PASS {args.bytes} bytes each direction; received SHA256 "
          f"{hashlib.sha256(incoming).hexdigest()}; {time.monotonic()-started:.2f}s", flush=True)
    if args.role == "dial":
        time.sleep(1.2)
        s.write(b"+++")
        wait_for(b"OK")
        command("AT+PHONUM?")
        command("ATH")
    else:
        wait_for(b"NO CARRIER")
    print("PASS escape/hangup", flush=True)
    command("AT+STATS?")
except Exception:
    time.sleep(1.2)
    s.write(b"+++")
    time.sleep(1.2)
    s.write(b"\rAT+STATS?\r")
    time.sleep(0.3)
    print("diagnostics:", repr(s.read(4096)), flush=True)
    raise
finally:
    s.close()
