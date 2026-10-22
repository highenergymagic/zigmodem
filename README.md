# Zigmodem — experimental 28.8k encrypted radio modem

Hayes-style encrypted serial radio modems for SONOFF ZBDongle-P (CC2652P) and
ZBDongle-E (EFR32MG21). Dial another dongle's numeric phone number, answer
its call, and exchange a reliable binary stream over 2.4 GHz IEEE 802.15.4.

This is experimental firmware, not a telephone-network modem. Both peers must
run compatible Zigmodem firmware and be within radio range. Version 0.4 is
incompatible with earlier firmware; upgrade both endpoints.

## Use

Open the USB serial port at **115200 baud, 8N1**, with hardware and software
flow control disabled. Allow about half a second after opening: some USB
bridge revisions reset the radio when the port opens. Keep the port open for
the duration of a call.

```
AT
OK
AT+PHONUM?
8056127476
OK
ATDT3793723694
CONNECT 28800
```

On the receiving modem:

```
RING
ATA
CONNECT 28800
```

After `CONNECT`, bytes pass to the other modem. To return to command mode,
pause transmission for one second, send `+++`, and pause for another second.
`ATH` hangs up; `ATO` returns to the connection. `ATS0=1` answers the first
ring automatically. Set `ATE0` to disable command echo.
`CONNECT n` reports the trained target payload rate; the UART remains at 115200.
The default maximum is **28800 bit/s**, with automatic fallback. This is a
one-direction rating: simultaneous streams share airtime, and scans, retries
and protocol overhead reduce useful throughput. Higher profiles require an
explicit `AT+MS` setting and remain experimental; 56k has not been established.

| Command | Meaning |
| --- | --- |
| `AT` | Check the modem |
| `ATI`, `ATI0` ... `ATI4` | Firmware identification and self-test responses |
| `AT+GMI`, `AT+GMM`, `AT+GMR`, `AT+GCAP` | Manufacturer, model, revision and capabilities |
| `AT+FCLASS?`, `AT+FCLASS=?`, `AT+FCLASS=0` | Data-only service; fax classes rejected |
| `AT+CALLCH?` | Calling channel (25) |
| `AT+SURVEY?` | Caller’s combined energy measurements, ages and move count |
| `AT+SCAN`, `AT+SCAN=0/1` | Request a survey / disable or enable caller-initiated surveys |
| `AT+PERF?` | Cumulative transmit radio/crypto microseconds and radio bytes |
| `AT+PHONUM?` | Ten-digit decimal phone number |
| `AT+IDENT?` | Full 128-bit public-key fingerprint, 32 hex digits |
| `AT+PIN=<fingerprint>` | Store a verified peer identity; refuse conflicting existing pins |
| `AT+BOOK?` | List persistent speed-dial slots, numbers and fingerprints |
| `ATDT1` ... `ATDT16` | Dial a stored full identity by its local numeric slot |
| `AT+STATS?` | Radio frame/retry counts, byte counts, overruns, last failure and RSSI |
| `AT+LINK?` | Current target payload rate, channel, training/recovery counters and RTT |
| `AT+SEC?` | Cipher suite, authenticated/rejected record counts, compression savings |
| `AT+WIRE?` | Wire-frame receive diagnostics |
| `AT+CHANNEL=n` | Retune to a channel in the scan list; active calls recover automatically |
| `AT+MS?`, `AT+MS=auto,min,max` | Link rate policy; auto is 0/1; rates: 300,1200,2400,4800,9600,14400,19200,28800,33600,56000,64000,115200; default `1,300,28800` |
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
| `ATS2=n`, `ATS2?` | Escape character; default 43 (`+`), values 128–255 disable escape |
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
flash partition. The full identity is the first 128 bits of BLAKE2s(public key).
A ten-digit number derived from it provides ordinary numeric dialing. The
Noise XX handshake encrypts and authenticates calls with ChaCha20-Poly1305;
fresh ephemeral keys provide forward secrecy. There is no plaintext fallback.

**Numeric dialing uses trust-on-first-use.** The modem remembers the peer's full
identity and rejects later key changes. A short number alone cannot authenticate
an unknown peer against a first-contact impersonator. Preload a verified identity
with `AT+PIN=<fingerprint>` or dial the verified full fingerprint for that first
call. Both reject a conflicting existing pin. No shared password is required.
Anyone may call; the pin book is not a caller access-control list.

Pins persist across resets and application upgrades. `AT+BOOK?` lists up to
16 entries. Each also has a short local alias: `ATDT1` calls entry 1 using its
full pinned identity. Empty slots return ERROR; a full book refuses new peers.
For the bench pair, carbon is **3793723694**, millie is **8056127476**, and
each has the other securely preloaded in slot **1**. Thus `ATDT1` works from
either host, including applications limited to very short digit-only numbers.

Initial training probes the agreed rate range and falls back when probes fail.
The target payload rates are pacing policies, not changes to the fixed
250 kbit/s IEEE 802.15.4 PHY or the host's UART baud rate. Clean acknowledged
traffic allows increases; repeated retries cause reductions. Lost links scan
channels 25,15,20,26,11 and retrain while retaining stream sequence/queue state.
The call ends if the peer remains absent for 30 seconds.

Idle modems meet on calling channel 25. After connection, authenticated
messages reserve short quiet slots to measure energy on channels 25,15,20,26,11.
An initial sweep is followed by one candidate every ten seconds. The caller
uses the noisier measurement from the two endpoints and moves both radios
only when a fresh alternative is over 6 dB quieter. Moving retrains without
discarding the byte stream. Scan slots briefly pause traffic; this is periodic
sampling, not continuous interference monitoring or a separate control radio.
A jammed calling channel can prevent new calls. `AT+SCAN=0` on the caller
disables proactive scans while retaining lost-link recovery.

Heatshrink compression runs before encryption, with a fresh dictionary per
record. A record is compressed only when that reduces its size. Already
compressed/encrypted data usually gains nothing. `AT%C0` disables compression;
ciphertext length still reveals traffic size, and compression can reveal
information when secrets and attacker-controlled bytes share a block.

Radio CRC, authenticated tags, and a four-packet selective-repeat window reject
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
  sustained serial input to measured link capacity, or use software flow control.
  The 115200 UART speed and CONNECT target do not guarantee radio throughput. UART overrun terminates the call
  instead of silently dropping data. The hardware test supports pacing or XON/XOFF.
  Optional `AT&K4` reserves XON/XOFF bytes (`0x11`/`0x13`) for local flow control;
  enable matching host flow control and use a transport that escapes these
  bytes (such as appropriately configured PPP). It is not transparent for an
  arbitrary unescaped binary stream. `AT&K0` restores byte transparency.
- Settings reset at boot; `AT&W` and stored profiles are not implemented.
- Linux `chat`/`pppd` 2.5.2 has passed LCP/IPCP, ping and binary TCP tests
  using the reset-safe serial bridge; see [PPP setup](docs/ppp.md). This is
  not full Hayes compatibility or qualification of every legacy application.
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
`crypto` tests exercise the v4 encrypted transport, training, channel recovery,
compression, replay rejection and independent Noise interoperability vectors.

## Firmware

Version **0.4** is classified as an **experimental 28.8k modem**. The default
rate policy is adaptive 300–28800, including after ATZ/AT&F. Rates above 28800
remain opt-in experiments; the earlier ~30 kbit/s one-way measurement does
not establish 56k service or guaranteed useful throughput under every policy.

The final default-policy P/E test transferred 32 KiB of incompressible data
simultaneously in each direction, verified every byte, performed six combined
energy measurements and one coordinated channel move/retrain, and recorded
zero UART overruns. Initial CONNECT reports were 28800 and 19200; directional
training is independent. Throughput including scan pauses was 12.1–12.6 kbit/s
per direction. A separate real PPP test passed LCP/IPCP negotiation, ten pings
with no loss, and a 32 KiB binary TCP echo in both directions (36.84 seconds).
These results describe the test pair, not a guaranteed minimum.

Version **0.3** uses 115200-baud serial ports, DMA on E, compact encrypted
packets and an eight-packet selective-repeat window. Bench measurements on
one P/E pair with 64 KiB of incompressible input were approximately **37–39
kbit/s one-way**, or **21 kbit/s per direction simultaneously** (about 43
kbit/s combined). These measurements used XON/XOFF and a fixed 56000 pacing
profile; they do **not** establish 56 kbit/s payload throughput. Automatic
training may select lower profiles. Every received byte was checked.

The 0.3 checks also cover numeric/one-digit dialing, persistent identity pins,
arbitrary binary transfers with software flow control off, manual retraining,
and channel recovery. The test excludes XON/XOFF bytes only when software
flow control is enabled. The host suite checks changed-pin rejection and a
hangup acknowledgment arriving midway through the next AT command.

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
Version 0.1 used 9600 baud because 115200 caused an EFR32 UART overrun.
Version 0.3 adds DMA RX/TX on E and uses 115200 on both boards.

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

For sustained incompressible throughput testing, use
`--bytes 65536 --flow --unpaced` on both ends.
Add `--direction dial` to test only
caller-to-answerer traffic; omit it for simultaneous duplex. The flow-control
test excludes reserved XON/XOFF bytes from deterministic random data and checks
every received byte. Its reported payload rate includes transfer/barrier time,
not dialing or training. Arbitrary binary qualification uses flow control off.
