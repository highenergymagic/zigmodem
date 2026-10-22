#!/usr/bin/env python3
"""Observe a call handshake; run answer first, then dial with a fingerprint."""
import argparse
import time
import serial
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("port")
p.add_argument("number", nargs="?")
a = p.parse_args()
s = serial.Serial(port=None, baudrate=9600, timeout=0.1, exclusive=True)
s.dtr = s.rts = False
s.port = a.port
s.open()
try:
    time.sleep(0.5)
    s.write(b"ATE0\rATS0=1\rATS7=60\r")
    time.sleep(0.5)
    print(repr(s.read(4096)), flush=True)
    if a.number:
        s.write(b"ATDT" + a.number.encode() + b"\r")
    started = last = time.monotonic()
    connected = False
    while time.monotonic() - started < 40:
        data = s.read(4096)
        if data:
            print(round(time.monotonic()-started,1), repr(data), flush=True)
            if b"CONNECT" in data:
                connected = True
        if time.monotonic()-last >= 5 and not connected:
            s.write(b"AT+SEC?\rAT+WIRE?\rAT+LINK?\r")
            last = time.monotonic()
    if connected:
        time.sleep(1.2)
        s.write(b"+++")
        time.sleep(1.2)
    s.write(b"AT+SEC?\rAT+WIRE?\rAT+LINK?\rATH\r")
    time.sleep(0.5)
    print(repr(s.read(4096)), flush=True)
finally:
    s.close()
