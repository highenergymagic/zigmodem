#!/usr/bin/env python3
"""Append Gecko application properties and package a Zephyr .bin as a GBL.

Uses the vendor Commander tool to add/verify application CRC and generate GBL.
The existing 16 KiB bootloader is excluded from the resulting image.
Format: Silicon Labs platform/bootloader/api/application_properties.h (v1.2).
"""
import argparse
from pathlib import Path
import struct
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("binary", type=Path)
p.add_argument("output", type=Path)
p.add_argument("--commander", required=True)
args = p.parse_args()
base = 0x4000
image = bytearray(args.binary.read_bytes())
if len(image) < 64 or len(image) > 700 * 1024:
    raise SystemExit("Unexpected application size")
sp, reset = struct.unpack_from("<II", image)
if not 0x20000000 < sp <= 0x20010000 or not base <= (reset & ~1) < base + len(image):
    raise SystemExit("Vector table does not describe an MG21 application at 0x4000")
image.extend(b"\xff" * (-len(image) % 4))
properties_address = base + len(image)
struct.pack_into("<I", image, 13 * 4, properties_address)
image.extend(struct.pack(
    "<16s6I16s2I16s",
    bytes.fromhex("13b779fac925ddb7adf3cfe0f1b614b8"),
    0x0201, 0, 0, 16, 0x00010000, 0,
    b"Zigmodem-MG21-v1\0", 0, 0, bytes(16),
))
raw = args.output.with_suffix(".app.bin")
hexfile = args.output.with_suffix(".app.hex")
raw.write_bytes(image)
subprocess.run([args.commander, "convert", str(raw), "--address", "0x4000", "--crc",
                "--outfile", str(hexfile)], check=True)
subprocess.run([args.commander, "gbl", "create", str(args.output), "--app", str(hexfile)], check=True)
print(f"Application properties at {properties_address:#x}; bootloader excluded")
