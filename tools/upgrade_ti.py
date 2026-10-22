#!/usr/bin/env python3
"""Upgrade ZBDongle-P by populated HEX pages, preserving identity storage."""
import argparse
import binascii
from intelhex import IntelHex
from cc2538_bsl.cc2538_bsl import CommandInterface, CC26xx

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("port")
p.add_argument("hex")
a = p.parse_args()
image = IntelHex(a.hex)
addresses = image.addresses()
if not addresses or any(x >= 0x58000 or 0x50000 <= x < 0x54000 for x in addresses):
    raise SystemExit("Image overlaps identity storage or exceeds flash")
pages = sorted({x // 8192 * 8192 for x in addresses})
cmd = CommandInterface()
cmd.open(a.port, 500000)
try:
    cmd.invoke_bootloader(sonoff_usb=True)
    if not cmd.sendSynch():
        raise RuntimeError("Bootloader did not respond")
    device = CC26xx(cmd)
    if device.size != 0x58000:
        raise RuntimeError("Unexpected flash size")
    storage_crc = device.crc(0x50000, 0x4000)
    for address in pages:
        data = image.tobinstr(start=address, size=8192)
        if not device.eraseRange(address, 8192) or not cmd.writeMemory(address, data, True):
            raise RuntimeError(f"Write failed at {address:#x}")
        if device.crc(address, 8192) != binascii.crc32(data) & 0xffffffff:
            raise RuntimeError(f"Verify failed at {address:#x}")
    if device.crc(0x50000, 0x4000) != storage_crc:
        raise RuntimeError("Identity storage changed unexpectedly")
    print(f"Verified {len(pages)} pages; identity storage unchanged")
finally:
    cmd.cmdReset()
    cmd.close()
