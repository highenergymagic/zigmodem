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

## ZBDongle-P

CC2652P, 352 KiB flash, 80 KiB SRAM. UART RX DIO12 / TX DIO13. The TI ROM
bootloader and active-low DIO15 backdoor remain enabled in the image's CCFG.
Use the USB bridge's SONOFF-specific reset sequence.

```sh
pip install cc2538-bsl pyserial
python tools/backup_ti.py /dev/ttyUSB1 /safe/private/location/original-ti.bin
cc2538-bsl -p /dev/ttyUSB1 --bootloader-sonoff-usb -ewv build-p/zephyr/zephyr.hex
python tools/serial_probe.py /dev/ttyUSB1
```

The backup helper reads 128-byte chunks and compares the complete image with
the bootloader's CRC **before** restarting the old application. The old Zigbee
application can update NV counters at boot, so a later whole-flash CRC can
differ from a correct backup. Backups may contain network keys: keep them
private and out of Git.

Restore a verified full binary with the same `-ewv` command, replacing the
HEX path with the backup path. This erases flash, writes the backup, verifies
it, and restarts the device. Keep the original CCFG intact in a recovery image.

## ZBDongle-E

EFR32MG21A020F768, 768 KiB flash, 64 KiB SRAM. UART USART0 PB0 RX / PB1 TX.
The application UART runs at 9600 baud; the Gecko bootloader still uses 115200.
At 115200, hardware RX overruns occurred during real duplex radio transfers.
The interrupt-driven UART backend does not yet use DMA or flow control.
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
The build workspace must contain `build-p`, `build-e`, `zephyr`, and `modules`.

- [TI board mapping and bootloader pin](https://github.com/Koenkk/Z-Stack-firmware/blob/master/coordinator/Z-Stack_3.x.0/README.md)
- [SONOFF E board configuration](https://github.com/darkxst/silabs-firmware-builder/blob/SiSDK/manifests/sonoff/zbdonglee_zigbee_ncp.yaml)
- [Gecko application memory layout](https://docs.silabs.com/mcu-bootloader/3.3.2/bootloader-fundamentals/03-memory-space-for-bootloading)
- [Gecko application properties](https://github.com/SiliconLabs/gecko_sdk/blob/gsdk_4.4/platform/bootloader/api/application_properties.h)
- [TI bootloader utility](https://github.com/JelmerT/cc2538-bsl)
- [Silicon Labs serial flasher](https://github.com/NabuCasa/universal-silabs-flasher)
