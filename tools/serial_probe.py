#!/usr/bin/env python3
"""Query a Zigmodem without changing DTR/RTS (which control reset on SONOFF)."""
import argparse
import time
import serial

p = argparse.ArgumentParser()
p.add_argument("port")
p.add_argument("commands", nargs="*", default=["AT", "ATI", "AT+PHONUM?"])
args = p.parse_args()
s = serial.Serial(port=None, baudrate=9600, timeout=0.25, exclusive=True)
s.dtr = False
s.rts = False
s.port = args.port
s.open()
try:
    startup = s.read(4096)
    if startup:
        print("startup:", repr(startup))
    for command in args.commands:
        s.write(command.encode("ascii") + b"\r")
        time.sleep(0.15)
        print(command, "=>", repr(s.read(4096)))
finally:
    s.close()
