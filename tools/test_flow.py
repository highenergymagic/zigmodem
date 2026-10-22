#!/usr/bin/env python3
"""Check explicit host XOFF/XON pause/resume at the firmware UART."""
import argparse
import time
import serial
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("port")
a = p.parse_args()
s = serial.Serial(port=None, baudrate=9600, timeout=0.2, exclusive=True)
s.dtr = s.rts = False
s.port = a.port
s.open()
try:
    time.sleep(0.5)
    s.reset_input_buffer()
    s.write(b"ATE0\rAT&K4\r")
    time.sleep(0.2)
    assert s.read(4096).count(b"OK") == 2
    s.write(b"\x13")
    time.sleep(0.1)
    s.write(b"AT\r")
    assert not s.read(4096), "UART transmitted despite host XOFF"
    s.write(b"\x11")
    assert b"OK" in s.read(4096), "UART did not resume on XON"
    s.write(b"AT&K0\r")
    assert b"OK" in s.read(4096)
    print("PASS XOFF pauses output; XON resumes without losing the pending result")
finally:
    s.write(b"\x11AT&K0\r")
    time.sleep(0.1)
    s.close()
