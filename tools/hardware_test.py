#!/usr/bin/env python3
"""Two-host radio integration test; run answer first, then dial on the other host.

SSH carries only test control/output. All test payload crosses the dongle radios.
Uses a paced stream because the stock boards do not expose modem flow control.
"""
import argparse
import hashlib
import re
import time
import threading
import serial

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("port")
p.add_argument("role", choices=["answer", "dial"])
p.add_argument("--number", help="Remote AT+PHONUM? value; needed for dial")
p.add_argument("--bytes", type=int, default=4096)
p.add_argument("--pattern", choices=["binary", "repeat"], default="binary")
p.add_argument("--retrain", action="store_true")
p.add_argument("--switch", action="store_true", help="force one peer off-channel and verify automatic recovery")
p.add_argument("--baud", type=int, default=115200)
p.add_argument("--flow", action="store_true", help="XON/XOFF; test data excludes reserved control bytes")
p.add_argument("--direction", choices=["duplex", "dial", "answer"], default="duplex")
p.add_argument("--fixed-rate", type=int)
p.add_argument("--unpaced", action="store_true", help="requires --flow")
p.add_argument("--no-scan", action="store_true")
p.add_argument("--no-compression", action="store_true")
args = p.parse_args()
if args.unpaced and not args.flow: p.error("--unpaced requires --flow")
s = serial.Serial(port=None, baudrate=args.baud, timeout=0.05, write_timeout=10, exclusive=True)
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
        if token == b"OK" and b"\r\nERROR\r\n" in data:
            raise RuntimeError(f"AT command rejected: {data!r}")
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
    if args.no_scan: command("AT+SCAN=0")
    if args.no_compression: command("AT%C0")
    if args.fixed_rate: command(f"AT+MS=0,{args.fixed_rate},{args.fixed_rate}")
    if args.flow:
        command("AT&K4")
        s.xonxoff = True
    if args.role == "answer":
        command("ATS0=0")
        print("READY", flush=True)
        wait_for(b"RING", 120)
        connected = command("ATA", b"CONNECT")
    else:
        if not args.number:
            raise ValueError("dial requires --number")
        connected = command("ATDT" + args.number, b"CONNECT")
    # CONNECT reports the payload pacing policy, independently of UART speed.
    match = re.search(rb"CONNECT (\d+)", connected)
    rate = int(match.group(1)) if match else 300
    own = 17 if args.role == "dial" else 29
    other = 29 if args.role == "dial" else 17
    outgoing = bytes((i * own + i // 256) % 256 for i in range(args.bytes))
    expected = bytes((i * other + i // 256) % 256 for i in range(args.bytes))
    if args.pattern == "repeat":
        outgoing = bytes([own]) * args.bytes
        expected = bytes([other]) * args.bytes
    if args.flow:
        # Deterministic incompressible input, excluding the UART's two reserved bytes.
        outgoing = hashlib.shake_256(bytes([own])).digest(args.bytes).replace(b"\x11",b"\x12").replace(b"\x13",b"\x14")
        expected = hashlib.shake_256(bytes([other])).digest(args.bytes).replace(b"\x11",b"\x12").replace(b"\x13",b"\x14")
    if args.direction != "duplex":
        if args.direction == args.role: expected = b""
        else: outgoing = b""
    incoming = bytearray()
    sent = 0
    failures = []
    started = time.monotonic()
    def writer():
        global sent
        try:
            time.sleep(0.5)
            while sent < len(outgoing):
                n = s.write(outgoing[sent:sent+64]); sent += n
                if not args.unpaced: time.sleep(n * 8 / rate * 1.1)
        except Exception as exc:
            failures.append(exc)
    sending = threading.Thread(target=writer, daemon=True)
    sending.start()
    deadline = started + max(120, args.bytes * 8 / 1200 * 2)
    while len(incoming) < len(expected) or sending.is_alive():
        if failures: raise failures[0]
        if time.monotonic() > deadline:
            raise RuntimeError(f"Transfer timeout: sent {sent}, received {len(incoming)}")
        if len(incoming) < len(expected): incoming.extend(s.read(min(512,len(expected)-len(incoming))))
        else: time.sleep(0.005)
    if failures: raise failures[0]
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
    print(f"PASS sent={len(outgoing)} received={len(expected)} bytes; received SHA256 "
          f"{hashlib.sha256(incoming).hexdigest()}; {time.monotonic()-started:.2f}s; "
          f"payload={max(len(outgoing),len(expected))*8/(time.monotonic()-started)/1000:.2f} kbit/s", flush=True)
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
    command("AT+PERF?")
    command("AT+SURVEY?")
except Exception:
    time.sleep(1.2)
    s.write(b"+++")
    time.sleep(1.2)
    s.write(b"\rAT+STATS?\rAT+LINK?\rAT+SEC?\r")
    time.sleep(0.3)
    print("diagnostics:", repr(s.read(4096)), flush=True)
    raise
finally:
    s.xonxoff = False
    s.write(b"\x11AT&K0\r")
    time.sleep(0.1)
    s.close()
