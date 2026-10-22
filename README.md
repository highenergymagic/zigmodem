# Zigmodem

Hayes-style encrypted serial radio modems for SONOFF ZBDongle-P (CC2652P) and
ZBDongle-E (EFR32MG21). Dial another dongle's public-key fingerprint, answer
its call, and exchange a reliable binary stream over 2.4 GHz IEEE 802.15.4.

This is experimental firmware, not a telephone-network modem. Both peers must
run compatible Zigmodem firmware and be within radio range. Version 0.2 does
not connect to plaintext 0.1 firmware.

## Use

Open the USB serial port at **9600 baud, 8N1**, with hardware and software
flow control disabled. Allow about half a second after opening: some USB
bridge revisions reset the radio when the port opens. Keep the port open for
the duration of a call.

```
AT
OK
AT+PHONUM?
F4A59EC16C3C605CFC17EED30E56D896
OK
ATDT2E9944F60F8F18FE7F59E33F1FFD9E56
CONNECT 9600
```

On the receiving modem:

```
RING
ATA
CONNECT 9600
```

After `CONNECT`, bytes pass to the other modem. To return to command mode,
pause transmission for one second, send `+++`, and pause for another second.
`ATH` hangs up; `ATO` returns to the connection. `ATS0=1` answers the first
ring automatically. Set `ATE0` to disable command echo.
`CONNECT n` reports the trained target payload rate; the UART remains at 9600.

| Command | Meaning |
| --- | --- |
| `AT` | Check the modem |
| `ATI` | Firmware identification |
| `AT+PHONUM?` | 128-bit public-key fingerprint, 32 hexadecimal digits |
| `AT+STATS?` | Radio frame/retry counts, byte counts, overruns, last failure and RSSI |
| `AT+LINK?` | Current target payload rate, channel, training/recovery counters and RTT |
| `AT+SEC?` | Cipher suite, authenticated/rejected record counts, compression savings |
| `AT+WIRE?` | Wire-frame receive diagnostics |
| `AT+CHANNEL=n` | Retune to a channel in the scan list; active calls recover automatically |
| `AT+MS?`, `AT+MS=auto,min,max` | Link rate policy; auto is 0/1; rates: 300,1200,2400,4800,9600 |
| `AT%C0/1` | Disable/enable outgoing block compression (default enabled) |
| `AT&K0/4`, `AT&K?` | Flow control off / XON-XOFF, or query; default off |
| `ATDT<number>`, `ATDP<number>` | Dial a peer |
| `ATA` | Answer an incoming call |
| `ATH`, `ATH0` | Hang up |
| `ATO`, `ATO0` | Return from command mode to data mode |
| `ATO1` | Retrain the current connection, then return to data mode |
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

## Encryption, training, compression, and recovery

First boot generates a random X25519 identity key and stores it in a reserved
flash partition. The phone number is the first 128 bits of BLAKE2s(public key).
The caller verifies the full fingerprint during a Noise XX handshake; every
subsequent modem frame is authenticated and encrypted with ChaCha20-Poly1305.
There is no plaintext fallback and no shared password to configure. Fresh
ephemeral keys provide forward secrecy between calls. Anyone who knows the
number may call; it is an identity, not an access-control list.

Initial training probes the agreed rate range and falls back when probes fail.
The target payload rates are pacing policies, not changes to the fixed
250 kbit/s IEEE 802.15.4 PHY or the host's UART baud rate. Clean acknowledged
traffic allows increases; repeated retries cause reductions. Lost links scan
channels 25,15,20,26,11 and retrain while retaining stream sequence/queue state.
The call ends if the peer remains absent for 30 seconds.

Heatshrink compression runs before encryption, with a fresh dictionary per
record. A record is compressed only when that reduces its size. Already
compressed/encrypted data usually gains nothing. `AT%C0` disables compression;
ciphertext length still reveals traffic size, and compression can reveal
information when secrets and attacker-controlled bytes share a block.

Radio CRC, authenticated record tags, and stop-and-wait acknowledgments reject
corruption and recover dropped data through retransmission. This is ARQ error
recovery, not an added forward-error-correction code. Duplicates are not delivered
twice. See [protocol details](docs/protocol.md).

## Current limits

- One direct call per radio; shared channel list, PAN ID `0x5a4d`, power 5 dBm.
- Experimental integration, not independently security-audited. Identity keys
  reside in ordinary flash; physical/debug access can expose them. Encryption
  cannot prevent radio jamming or denial of service during discovery.
- No mesh routing, discovery directory, audio, or PSTN access.
- No DCD/RI/DTR modem semantics or RTS/CTS flow control. Stock bridge wiring
  limits these features: DTR/RTS operate reset/bootloader circuitry. Pace
  sustained serial input to the negotiated rate; 9600 is the UART
  speed, not a guaranteed radio throughput. UART overrun terminates the call
  instead of silently dropping data. The hardware test uses paced writes.
  Optional `AT&K4` reserves XON/XOFF bytes (`0x11`/`0x13`) for local flow control;
  enable matching host flow control and use a transport that escapes these
  bytes (such as appropriately configured PPP). It is not transparent for an
  arbitrary unescaped binary stream. `AT&K0` restores byte transparency.
- Settings reset at boot; `AT&W` and stored profiles are not implemented.
- PPP and legacy modem applications have not yet been qualified. The
  transport carries arbitrary bytes, but that alone is not full Hayes or
  hardware modem compatibility.
- Firmware updates replace the dongle's Zigbee function. Preserve a recovery
  image and network backup before flashing. Never flash the P image onto E
  or vice versa. For P upgrades use `tools/upgrade_ti.py`: mass erase destroys
  the persistent identity and changes the phone number.

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
This legacy simulator runs the plaintext modem core only. The `secure` and
`crypto` tests exercise the v2 encrypted transport, training, channel recovery,
compression, replay rejection and independent Noise interoperability vectors.

## Firmware

The **0.2 encrypted** bench qualification used one P/E pair on separate hosts:

- P dialing E: 4 KiB of binary data in each direction, byte-for-byte checked;
  forced off-channel operation recovered automatically and delivered more data
  within the same call. Initial training reported 9600 in both directions;
  rates changed during transfer/recovery, including a fallback to 300.
- E dialing P: 4 KiB of compressible data in each direction, byte-for-byte
  checked, followed by `ATO1` retraining and another data exchange.
- Both runs exercised guarded escape and hangup without UART overruns.
  Separate checks verified XON/XOFF pauses and resumes serial output on both
  boards. Persistent phone numbers survived repeated application upgrades.

These are bench checks, not a range/endurance rating or a security audit.
Host tests cover lost/duplicated frames, replay and forged-counter rejection,
wrong destination fingerprints, channel recovery, bounded compression, and
independent Noise interoperability vectors. Both native and microcontroller
Curve25519 arithmetic paths pass AddressSanitizer/UndefinedBehaviorSanitizer.

The **0.1 plaintext** hardware qualification passed 32 KiB in each direction with E
dialing P, and 4 KiB in each direction with P dialing E. Both runs checked
every byte, guarded escape, command mode, and hangup with zero UART overruns.
The longer run took 52 seconds (about 625 bytes/s per direction with the test's
pacing). This is a bench test of one pair, not a range or endurance rating.
Channel 20 suffered repeated clear-channel-assessment failures in that setup;
channel 25 worked. Version 0.2 corrects the TI driver's CCA configuration;
the earlier channel observation does not characterize the new firmware.
The application uses 9600 baud because 115200 caused an
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
Add `--switch` to both commands to exercise channel recovery, or
`--pattern repeat --retrain` to check compression and manual retraining.
