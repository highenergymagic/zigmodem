#!/usr/bin/env python3
"""Binary stdio bridge for pppd's pty option on SONOFF reset-wired USB UARTs.

Keeps DTR/RTS deasserted; chat must enable AT&K4 and PPP must escape 11/13.
Diagnostics go to stderr, never the PPP stream. Requires pyserial.
"""
import argparse
import os
import threading
import time
import serial

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("port")
args = p.parse_args()
s = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=30,
                  xonxoff=True, exclusive=True)
s.dtr = s.rts = False
s.port = args.port
s.open()
try:
    time.sleep(0.5)
    s.reset_input_buffer()
    def receive():
        try:
            while s.is_open:
                data = s.read(512)
                while data:
                    data = data[os.write(1, data):]
        except (OSError, serial.SerialException):
            os._exit(1)
    threading.Thread(target=receive, daemon=True).start()
    while True:
        data = os.read(0, 512)
        if not data:
            break
        s.write(data)
finally:
    s.close()
