#!/usr/bin/env python3
"""Read-only TI flash backup, CRC-verified BEFORE restarting the application.

Requires cc2538-bsl. The original application can update its NV counters on
boot, so verifying in a separate bootloader invocation is not sufficient.
"""
import argparse
import binascii
from pathlib import Path
from cc2538_bsl.cc2538_bsl import CommandInterface, CC26xx

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("port")
p.add_argument("output", type=Path)
args = p.parse_args()
if args.output.exists():
    raise SystemExit("Refusing to overwrite existing backup")
cmd = CommandInterface()
cmd.open(args.port, 500000)
try:
    cmd.invoke_bootloader(sonoff_usb=True)
    if not cmd.sendSynch():
        raise RuntimeError("Bootloader did not respond")
    device = CC26xx(cmd)
    if device.size != 352 * 1024:
        raise RuntimeError(f"Unexpected flash size: {device.size}")
    data = bytearray()
    for addr in range(0, device.size, 128):
        cmd._write(9)
        cmd._write(cmd._calc_checks(0x2A, addr, 33))
        cmd._write(0x2A)
        cmd._write(cmd._encode_addr(addr))
        cmd._write(1)  # 32-bit accesses
        cmd._write(32)  # 128 bytes, below the response packet limit
        if not cmd._wait_for_ack("MEMORY_READ", 1):
            raise RuntimeError(f"Read failed at {addr:#x}")
        block = cmd.receivePacket()
        if not cmd.checkLastCmd() or len(block) != 128:
            raise RuntimeError(f"Short read at {addr:#x}")
        data.extend(block)
    expected = device.crc(0, len(data))
    actual = binascii.crc32(data) & 0xffffffff
    if actual != expected:
        raise RuntimeError(f"Backup CRC mismatch: {actual:08x} != {expected:08x}")
    args.output.write_bytes(data)
    args.output.chmod(0o600)
    print(f"Verified {len(data)} bytes; CRC32 {actual:08x}")
finally:
    cmd.cmdReset()
    cmd.close()
