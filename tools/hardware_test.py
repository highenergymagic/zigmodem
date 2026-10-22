#!/usr/bin/env python3
"""Two-host radio integration test; run answer first, then dial on the other host.

SSH carries only test control/output. All test payload crosses the dongle radios.
Uses a paced stream because the stock boards do not expose modem flow control.
"""
import argparse
import hashlib
import re
import time
import serial

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("port")
p.add_argument("role", choices=["answer", "dial"])
p.add_argument("--number", help="Remote AT+PHONUM? value; needed for dial")
p.add_argument("--bytes", type=int, default=4096)
p.add_argument("--pattern", choices=["binary", "repeat"], default="binary")
p.add_argument("--retrain", action="store_true")
p.add_argument("--switch", action="store_true", help="force one peer off-channel and verify automatic recovery")
args = p.parse_args()
s = serial.Serial(port=None, baudrate=9600, timeout=0.05, exclusive=True)
s.dtr = s.rts = False
s.port = args.port
s.open()

def wait_for(token, timeout=90):
    data = bytearray()
    framed = b"\r\n" + token + b"\r\n"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        # Never consume stream bytes following CONNECT in the same USB read.
        data.extend(s.read(1))
        if token == b"CONNECT" and any(x in data for x in (b"\r\nNO ANSWER\r\n", b"\r\nBUSY\r\n", b"\r\nNO CARRIER\r\n")):
            raise RuntimeError(f"Call failed: {data!r}")
        connected = token == b"CONNECT" and re.search(rb"\r\nCONNECT(?: \d+)?\r\n$", data)
        if framed in data or connected:
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
        connected = command("ATA", b"CONNECT")
    else:
        if not args.number:
            raise ValueError("dial requires --number")
        connected = command("ATDT" + args.number, b"CONNECT")
    # CONNECT reports DCE pacing; the host UART remains at 9600 baud.
    match = re.search(rb"CONNECT (\d+)", connected)
    rate = int(match.group(1)) if match else 300
    own = 17 if args.role == "dial" else 29
    other = 29 if args.role == "dial" else 17
    outgoing = bytes((i * own + i // 256) % 256 for i in range(args.bytes))
    expected = bytes((i * other + i // 256) % 256 for i in range(args.bytes))
    if args.pattern == "repeat":
        outgoing = bytes([own]) * args.bytes
        expected = bytes([other]) * args.bytes
    incoming = bytearray()
    sent = 0
    started = time.monotonic()
    next_send = started + 0.5
    deadline = started + max(90, args.bytes * 8 / 300 * 2)
    while len(incoming) < len(expected) or sent < len(outgoing):
        now = time.monotonic()
        if now > deadline:
            raise RuntimeError(f"Transfer timeout: sent {sent}, received {len(incoming)}; "
                               f"first={incoming[:180]!r}; last={incoming[-100:]!r}")
        if sent < len(outgoing) and now >= next_send:
            n = s.write(outgoing[sent:sent+32])
            sent += n
            next_send = now + max(0.02, n * 8 / rate * 1.25)
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
    if args.retrain:
        time.sleep(1.2)
        s.write(b"+++")
        wait_for(b"OK")
        command("ATO1", b"CONNECT")
        s.write(b"ZMRETRAIN")
        marker = bytearray()
        deadline = time.monotonic() + 30
        while len(marker) < 9 and time.monotonic() < deadline:
            marker.extend(s.read(9-len(marker)))
        if marker != b"ZMRETRAIN":
            raise RuntimeError(f"Post-retrain marker mismatch: {marker!r}")
        print("PASS manual retrain and subsequent data", flush=True)
    if args.switch:
        time.sleep(1.2)
        s.write(b"+++")
        wait_for(b"OK")
        state = command("AT+LINK?")
        previous = int(re.search(rb"RETRAINS=(\d+)", state).group(1))
        channel = int(re.search(rb"CHANNEL=(\d+)", state).group(1))
        if args.role == "dial":
            time.sleep(1)
            command("AT+CHANNEL=" + str(26 if channel != 26 else 25))
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            time.sleep(1)
            state = command("AT+LINK?")
            if int(re.search(rb"RETRAINS=(\d+)", state).group(1)) > previous and b"TRAINING=0 RECOVERING=0" in state:
                break
        else:
            raise RuntimeError("Channel recovery did not complete")
        command("ATO", b"CONNECT")
        s.write(b"ZMCHANNEL")
        marker = bytearray()
        deadline = time.monotonic() + 30
        while len(marker) < 9 and time.monotonic() < deadline:
            marker.extend(s.read(9-len(marker)))
        if marker != b"ZMCHANNEL":
            raise RuntimeError(f"Post-channel-recovery marker mismatch: {marker!r}")
        print("PASS forced channel loss, automatic rendezvous/retrain, and subsequent data", flush=True)
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
    command("AT+LINK?")
    command("AT+SEC?")
except Exception:
    time.sleep(1.2)
    s.write(b"+++")
    time.sleep(1.2)
    s.write(b"\rAT+STATS?\rAT+LINK?\rAT+SEC?\r")
    time.sleep(0.3)
    print("diagnostics:", repr(s.read(4096)), flush=True)
    raise
finally:
    s.close()
