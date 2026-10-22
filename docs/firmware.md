# Firmware builds and recovery

The manifest pins Zephyr to commit
`1179a68cd2d142b2dd93b76101eb24ae335a8e24` (4.5.0-rc2). Build on Linux with
Python 3.12+, CMake, Ninja, and Zephyr SDK 1.0.1's ARM GNU toolchain.

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install west
west init -l .
west update --fetch-opt=--depth=1
pip install -r ../zephyr/scripts/requirements-base.txt
west sdk install --gnu-toolchains arm-zephyr-eabi
west blobs fetch hal_silabs
python tools/prepare_zephyr.py ../zephyr
west build -b zbdongle_p firmware -d build-p
west build -b zbdongle_e firmware -d build-e
```

The preparation script applies narrowly scoped compatibility fixes to the
pinned radio drivers: TI custom-L2 registration/initialization, Silicon Labs
OpenThread-only headers and metadata, standalone PHY constants, and the
lifetime of its link-address buffer, and SONOFF's TI crystal load trim
(`0xC1`, measured from the original CCFG rather than the LaunchPad's `0xD5`).
It also resets TI's CSMA in/out backoff fields for each transmission and
exposes its command status for diagnostics. It refuses unexpected source patterns
and can be rerun. These changes are not upstreamed yet.
The TI receive CCA correlation operator is corrected from OR to AND to match
its declared CCA mode 3 and TI's reference driver. This removed repeated
channel-busy transmit failures in the hardware qualification.
Version 0.4 also exposes a forced TI receiver restart after energy detection;
without it the receiver can remain stopped after a channel-survey slot.
Both endpoints must run v0.4 for the new survey protocol. Application updates
retain the existing identity and phone book; settings default to adaptive
300–28800 bit/s with proactive surveys enabled.

## ZBDongle-P

CC2652P, 352 KiB flash, 80 KiB SRAM. UART RX DIO12 / TX DIO13. The TI ROM
bootloader and active-low DIO15 backdoor remain enabled in the image's CCFG.
Use the USB bridge's SONOFF-specific reset sequence.

```sh
pip install cc2538-bsl pyserial
python tools/backup_ti.py /dev/ttyUSB1 /safe/private/location/original-ti.bin
python tools/upgrade_ti.py /dev/ttyUSB1 build-p/zephyr/zephyr.hex
python tools/serial_probe.py /dev/ttyUSB1
```

The backup helper reads 128-byte chunks and compares the complete image with
the bootloader's CRC **before** restarting the old application. The old Zigbee
application can update NV counters at boot, so a later whole-flash CRC can
differ from a correct backup. Backups may contain network keys: keep them
private and out of Git.

To restore a verified full backup intentionally, use
`cc2538-bsl -p /dev/ttyUSB1 -evw --bootloader-sonoff-usb /safe/private/location/original-ti.bin`.
This erases flash, writes the backup and verifies it; reset the device afterward.
It replaces the identity too. Keep the original CCFG intact in a recovery image.

Versions 0.2 and later reserve `0x50000..0x53fff` for identity storage. Its application
region ends before this; CCFG remains at the chip's original address. The
upgrade helper erases/writes only populated 8 KiB HEX pages and verifies the
storage CRC is unchanged. **Do not mass erase for normal upgrades.** A full
restore or mass erase intentionally discards the current cryptographic identity.

## ZBDongle-E

EFR32MG21A020F768, 768 KiB flash, 64 KiB SRAM. UART USART0 PB0 RX / PB1 TX.
The application and Gecko bootloader both use 115200 baud. Version 0.3 uses
the USART asynchronous API with LDMA RX/TX and alternating receive buffers.
This replaces the interrupt-only E backend that previously overran at 115200.
The P retains its interrupt-driven UART backend. There is no hardware flow control.
Optional software XON/XOFF flow control is available with `AT&K4`.
Zephyr's A-series device declaration names the 1024 KiB variant; this board
explicitly constrains flash and RAM to the smaller physical chip. HFXO CTUNE
140 matches the inspected SONOFF firmware configuration; board revisions may
need their own calibration.

The application starts at **0x4000**, preserving the installed Gecko
bootloader. Do not flash this ELF/HEX as a full-chip replacement. Package an
application-only GBL with Silicon Labs Simplicity Commander:

```sh
python tools/package_gecko.py build-e/zephyr/zephyr.bin build-e/zigmodem.gbl \
  --commander /path/to/commander
pip install universal-silabs-flasher pyserial
universal-silabs-flasher --device /dev/ttyUSB0 \
  --bootloader-reset rts_dtr --probe-methods bootloader:115200 \
  flash --firmware build-e/zigmodem.gbl
python tools/serial_probe.py /dev/ttyUSB0
```

The packaging tool validates the vector table, appends Gecko application
properties, sets vector word 13, and asks Commander to add the application
CRC and construct the GBL. It never includes bootloader bytes.
Versions 0.2 and later reserve `0xb8000..0xbbfff` for identity storage. The application
link region ends at `0xb8000`; application-only upgrades preserve this partition.

## Persistent identity

Each device creates a private X25519 key using hardware entropy on its first
0.2-or-later boot, saves it with Zephyr NVS data CRC, and reads it back before use.
Subsequent boots use that key. `AT&F` does not delete it. Missing storage
initializes a new identity; corrupt/unreadable key records fail startup rather
than silently substituting a new key. The key is not embedded in build outputs,
published firmware, or repository files. Both actual boards retained their
phone numbers across repeated application upgrades during development.

The key is stored in ordinary flash, not a hardware secure element. Recovery
and debug access remain available; this does not provide physical-tamper security.
Losing/erasing the identity partition changes the phone number. Version 0.3
keeps the same key but exposes a ten-digit derived number through `AT+PHONUM?`;
`AT+IDENT?` still exposes the original full fingerprint. Sixteen full peer
fingerprints are stored in NVS IDs 0x100..0x10f beside the identity at ID 1.
`AT+PIN=<verified fingerprint>` provisions an entry; `AT+BOOK?` lists the entries.
Ordinary upgrades preserve all of them. `AT&F` does not clear them.

The tested CP2102N-based V2 board supports `rts_dtr` bootloader entry. Other
revisions may require the physical BOOT button. Verify entry before replacing
the existing application. Recovery uses the same flashing command with a
known-good vendor ZBDongle-E GBL. Network configuration can be backed up with
`zigpy radio --baudrate 115200 ezsp PORT backup FILE` before replacement.
The UART Gecko bootloader does not provide full application flash readback.

## Sources

Release firmware includes third-party code under its original licenses;
Zigmodem's MIT license applies to this repository's own code. Keep the
`third-party-notices.zip` release asset with redistributed firmware. To collect
dependency license files and notices from both completed build trees, run
`python tools/package_notices.py /path/to/build-workspace third-party-notices.zip`.
The build workspace must contain `build-p`, `build-e`, `zephyr`, `modules`,
`vendor`, and this project's `LICENSE`.

- [TI board mapping and bootloader pin](https://github.com/Koenkk/Z-Stack-firmware/blob/master/coordinator/Z-Stack_3.x.0/README.md)
- [TI reference CCA configuration](https://github.com/openthread/ot-cc13x2-cc26x2/blob/bdcd288ae64937172ec187415ed768875177f272/src/cc2652/radio.c#L263)
- [SONOFF E board configuration](https://github.com/darkxst/silabs-firmware-builder/blob/SiSDK/manifests/sonoff/zbdonglee_zigbee_ncp.yaml)
- [Gecko application memory layout](https://docs.silabs.com/mcu-bootloader/3.3.2/bootloader-fundamentals/03-memory-space-for-bootloading)
- [Gecko application properties](https://github.com/SiliconLabs/gecko_sdk/blob/gsdk_4.4/platform/bootloader/api/application_properties.h)
- [TI bootloader utility](https://github.com/JelmerT/cc2538-bsl)
- [Silicon Labs serial flasher](https://github.com/NabuCasa/universal-silabs-flasher)
