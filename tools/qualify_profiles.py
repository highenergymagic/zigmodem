#!/usr/bin/env python3
"""Hardware qualification: overwrite profiles 0/1, reflash externally, verify.

Stop all other serial users first. `prepare` saves native profile 0 and USR
profile 1, selecting profile 1 at boot. `verify` checks the booted profile and
restores native profile 0 as the boot selection. Both use software flow control.
"""
import argparse
import time
import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("phase", choices=("prepare", "verify"))
    args = parser.parse_args()
    s = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=5)
    s.dtr = s.rts = False
    s.port = args.port
    s.open()

    def command(text, expected=b"\r\nOK\r\n"):
        s.write(text.encode("ascii") + b"\r")
        data = bytearray()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            data.extend(s.read(1024))
            if b"\r\nOK\r\n" in data or b"\r\nERROR\r\n" in data:
                break
        print(text, "=>", repr(bytes(data)), flush=True)
        assert expected in data and b"\r\nERROR\r\n" not in data, text
        return bytes(data)

    try:
        time.sleep(0.6)
        s.reset_input_buffer()
        if args.phase == "prepare":
            command("AT+COMPAT=NATIVE")
            command("AT&F2E1V1Q0W1&W0")
            command("AT+COMPAT=USR")
            command("AT&F2E1V1Q0&A3S19=7&W1Y1")
            command("ATI5", b"STORED PROFILE 1 (BOOT)")
            print("Prepared: reflash the application, then run verify.")
        else:
            command("AT+COMPAT?", b"\r\nUSR\r\n")
            command("ATS19?", b"\r\n7\r\n")
            data = command("ATI4")
            assert b"REPORT=3" in data and b"FLOW_TX=1 FLOW_RX=1" in data
            command("ATI5", b"STORED PROFILE 1 (BOOT)")
            command("ATS19=0&W1")  # Remove the temporary test timeout.
            command("AT+COMPAT=NATIVE")
            command("AT&Y0Z")
            command("AT+COMPAT?", b"\r\nNATIVE\r\n")
            command("ATS19?", b"\r\n0\r\n")
            command("ATI4", b"REPORT=1")
            print("PASS: profile survived restart/reflash; native profile 0 restored.")
        command("AT+PHONUM?")
        command("AT+BOOK?")
    finally:
        s.close()


if __name__ == "__main__":
    main()
