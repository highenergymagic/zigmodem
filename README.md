# Zigmodem

Hayes-style serial radio modems for SONOFF ZBDongle-P (CC2652P) and
ZBDongle-E (EFR32MG21). Dial another dongle's factory-derived decimal number,
answer its call, and exchange a binary byte stream over 2.4 GHz IEEE 802.15.4.

This is experimental firmware, not a telephone-network modem. Both peers must
run Zigmodem, use the same radio channel, and be within radio range.

## Use

Open the USB serial port at **9600 baud, 8N1**, with hardware and software
flow control disabled. Allow about half a second after opening: some USB
bridge revisions reset the radio when the port opens. Keep the port open for
the duration of a call.

```
AT
OK
AT+PHONUM?
10714651752211049825
OK
ATDT5149013750468090
CONNECT
```

On the receiving modem:

```
RING
ATA
CONNECT
```

After `CONNECT`, bytes pass to the other modem. To return to command mode,
pause transmission for one second, send `+++`, and pause for another second.
`ATH` hangs up; `ATO` returns to the connection. `ATS0=1` answers the first
ring automatically. Set `ATE0` to disable command echo.

| Command | Meaning |
| --- | --- |
| `AT` | Check the modem |
| `ATI` | Firmware identification |
| `AT+PHONUM?` | Full 64-bit device address as an unsigned decimal number |
| `AT+STATS?` | Radio frame/retry counts, byte counts, overruns, last failure and RSSI |
| `ATDT<number>`, `ATDP<number>` | Dial a peer |
| `ATA` | Answer an incoming call |
| `ATH`, `ATH0` | Hang up |
| `ATO`, `ATO0` | Return from command mode to data mode |
| `ATZ`, `AT&F` | Hang up and restore default settings |
| `ATE0/1` | Command echo off/on |
| `ATV0/1` | Numeric/text result codes |
| `ATQ0/1` | Result codes on/off |
| `ATS0=n`, `ATS0?` | Auto-answer ring count; zero disables |
| `ATS7=n`, `ATS7?` | Dial timeout in seconds (1–255; default 30) |
| `ATS12=n`, `ATS12?` | Escape guard time in 20 ms units (1–255; default 50) |

Result codes: `OK`, `CONNECT`, `RING`, `NO CARRIER`, `ERROR`, `BUSY`,
`NO ANSWER`. Unsupported commands return `ERROR`; no pretend-success stubs.

`AT+STATS?` reports cumulative counters since boot. `FAULT=1` means UART
overrun, `2` means peer silence, and `3` means the serial result queue was
full; zero means no local fault. `RFERR` counts failed radio transmissions or
packet allocations. RSSI is the last received Zigmodem MAC frame's signal
strength in dBm (zero before any frame). Counters survive hangup, not reset.
On TI, `STATUS` contains the hexadecimal CSMA status followed by TX status:
`24002400` means both succeeded; `24010000` means channel busy, TX not started.
On Silicon Labs this diagnostic field is zero.

## Current limits

- One direct call per radio; channel 25, PAN ID `0x5a4d`, transmit power 5 dBm.
- No encryption or peer authentication yet. Numbers identify peers but are
  not credentials. Use this version for experiments with non-sensitive data.
- No mesh routing, discovery directory, compression, audio, or PSTN access.
- No DCD/RI/DTR modem semantics or UART flow control. Stock bridge wiring
  limits these features. Pace sustained serial input; 9600 is the UART
  speed, not a guaranteed radio throughput. UART overrun terminates the call
  instead of silently dropping data. The hardware test uses paced writes.
- Settings reset at boot; `AT&W` and stored profiles are not implemented.
- PPP and legacy modem applications have not yet been qualified. The
  transport carries arbitrary bytes, but that alone is not full Hayes or
  hardware modem compatibility.
- Firmware updates replace the dongle's Zigbee function. Preserve a recovery
  image and network backup before flashing. Never flash the P image onto E
  or vice versa.

## Build and test the portable core

Requires a C11 compiler and CMake. Linux also builds a PTY/UDP simulator.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

The deterministic tests exercise manual/automatic answer, 128 KiB aggregate
duplex binary transfer with dropped and duplicated packets, backpressure,
escape guard times, busy handling, connection loss, hangup retries, malformed
frames, and stale sessions.

Run two simulated modems in separate terminals:

```sh
build/zigmodem-sim 123 19001 19002
build/zigmodem-sim 456 19002 19001
```

Each prints a PTY path. Open each path with a terminal program, then dial
`ATDT456` from the first. Simulator UDP is bound to loopback; it is a test
transport, not the firmware's radio transport.

## Firmware

The first hardware qualification passed 32 KiB in each direction with E
dialing P, and 4 KiB in each direction with P dialing E. Both runs checked
every byte, guarded escape, command mode, and hangup with zero UART overruns.
The longer run took 52 seconds (about 625 bytes/s per direction with the test's
pacing). This is a bench test of one pair, not a range or endurance rating.
Channel 20 suffered repeated clear-channel-assessment failures in that setup;
channel 25 worked. The application uses 9600 baud because 115200 caused an
EFR32 UART overrun during sustained duplex traffic.

See [building and recovery](docs/firmware.md) and the
[wire protocol](docs/protocol.md). Both targets use the same C modem core and
Zephyr application. Board files supply the different UART, memory, clock,
and bootloader settings. Vendor radio libraries remain external dependencies
with their own licenses.

To reproduce the real-radio test, start the answer side first:

```sh
python tools/hardware_test.py /dev/ttyUSB0 answer
python tools/hardware_test.py /dev/ttyUSB1 dial --number <answer-side-number>
```

Run those commands on the machines hosting the respective dongles. They check
binary content in both directions, guarded escape, querying while connected,
and hangup. SSH may orchestrate them; it does not carry the test payload.
