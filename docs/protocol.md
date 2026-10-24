# Zigmodem encrypted wire protocol v5

This is a custom protocol over the 250 kbit/s IEEE 802.15.4 PHY, not a Zigbee
profile. Channels are 25,15,20,26,11; PAN is `0x5a4d`. A shared half-duplex
radio channel carries two logically simultaneous byte streams. Version 5 is
incompatible with earlier firmware. Version 5 expands the ordered rate table
to 19 entries. Capability indices refer to that table; upgrade both endpoints.

## Identity and dialing

A persistent random X25519 private key identifies each modem. Its full identity
is the first 16 bytes of BLAKE2s(public key), displayed by `AT+IDENT?` as 32 hex
digits. The phone number is `1000000000 + LE64(identity[0:8]) % 9000000000`.
It is always ten decimal digits and is returned by `AT+PHONUM?`.

Short numbers are routing addresses, not cryptographically strong fingerprints.
Numeric dialing pins the full authenticated identity on first use in NVS;
subsequent calls reject a different identity with the same number. This is
trust-on-first-use: a first-contact attacker who finds a matching short number
could impersonate the intended peer. Use `AT+PIN=<verified full fingerprint>`
beforehand, or dial a verified full fingerprint directly, to avoid that risk.
Full-fingerprint calls also respect existing pins. Pins do not form a caller ACL.

The sixteen persistent pin slots also provide local speed dialing: `ATDT1`
through `ATDT16`. `AT+BOOK?` lists slot, number and fingerprint. Aliases refer
to complete identities, never an unauthenticated directory lookup. An empty
slot fails, conflicting pins fail, and a full book refuses new identities.
Settings reset (`AT&F`) does not clear identities or pins.

## Radio envelope and handshake

The MAC header is 15 bytes: frame control `0xc841`, sequence, PAN, broadcast
destination `0xffff`, and 64-bit source containing the decimal phone number.
The radio generates/checks the FCS. There are no MAC ACKs; encrypted selective
acknowledgments provide reliability. All multibyte integers are little endian.

Handshake fragments use this header:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 2 | ASCII `ZS` |
| 2 | 1 | Version 5 |
| 3 | 1 | HS1=1, HS2=2, HS3=3 |
| 4 | 8 | Source number |
| 12 | 8 | Destination number |
| 20 | 8 | Session ID |
| 28 | 8 | Zero |
| 36 | 4 | Reserved, zero |
| 40 | 1 | Fragment index |
| 41 | 1 | Fragment count |
| 42 | 2 | Complete record length |
| 44 | 1–64 | Fragment bytes |

Reassembly is bounded to 160 bytes. Noise_XX_25519_ChaChaPoly_BLAKE2s uses
fresh ephemeral keys per call. Its prologue is `ZM5\0`, caller number, callee
number, and session ID. Sessions start from a random boot nonce plus a counter.
The caller verifies the callee after HS2; the callee verifies the caller after
HS3. Both apply persistent identity pins. Cached handshake messages retransmit
without advancing Noise state, with a 15-second timeout.

## Compact encrypted records

DATA=4 and READY=5 use a twelve-byte header: `ZS`, version 5, type, and a
64-bit directional counter. The entire header is AEAD associated data. Noise
keys already bind the session and identities, so full addresses are not repeated.
A 16-byte tag follows ciphertext. Maximum radio payload is 108 bytes, or 125
including MAC header and FCS. Ordinary data does not require fragmentation.

READY has empty plaintext and confirms the responder's transport key before
the initiator sends CALL. DATA plaintext is compression flag, two-byte decoded
length, then a compact modem message: type (1), sequence (4), ACK base (4),
ACK bitmap (4), and zero to 64 payload bytes. The secure layer reconstructs
source, destination and session from the authenticated connection state.

Heatshrink uses window bits=8, lookahead bits=4 and a fresh dictionary per
record, only when smaller. Decompression occurs after authentication and is
bounded to 77 decoded bytes. Compression can reveal information via lengths
when secrets and attacker-controlled bytes share a block; `AT%C0` disables it.

Directions have independent keys and increasing 64-bit counters. A 64-record
replay window advances only after successful authentication. Forged high
counters cannot poison it. Retransmissions use fresh counters. Power cycling
ends the session and requires a fresh handshake.

## Modem messages and call state

The core's in-memory envelope is `ZM`, version 1, type, source (8), destination
(8), session (8), sequence (4), ACK base (4), ACK bitmap (4), then payload.
Its header is 40 bytes and is never accepted directly over radio. The legacy
PTY simulator exercises this core without encryption or advanced windowing.

Types: CALL=1, ACCEPT=2, CONFIRM=3, BUSY=4, DATA=5, ACK=6, PING=7,
PONG=8, CLOSE=9, CLOSED=10, WAIT=11, PROBE=12, PROBE_ACK=13, SURVEY=14, POWER=15. CALL/ACCEPT
carry version=5, maximum profile, minimum profile; DATA carries 1–64 bytes.

CALL/ACCEPT repeat every 500 ms until confirmation. RING repeats every two
seconds; manual or automatic answer starts ACCEPT. DATA or PING can confirm
if CONFIRM was lost. CONNECT follows training. Only one encrypted session is
supported; additional callers normally time out instead of receiving BUSY.
CLOSE repeats every 300 ms for up to three seconds. PING/PONG exchange each
second; 30 seconds without contact closes a connected call.

## Training, recovery and stream

Profiles target 300,1200,2400,4800,7200,9600,12000,14400,16800,19200,21600,24000,26400,28800,31200,33600,56000,64000,115200 payload
bits/s. Defaults cap the maximum at 28800; higher profiles require opt-in. Eight full-size probes test the highest mutually allowed profile first.
Fewer than seven replies causes adaptive fallback; fewer than two replies at
the selected minimum fails the call. Training times out after 60 seconds.
These are pacing policies, not throughput guarantees or PHY modulation changes.
Every sixteen retries lowers the adaptive profile; 64 clean acknowledged
packets permit an increase. `ATO1` retrains explicitly.

Each direction has a four-packet selective-repeat window. ACK base is the
next sequence awaiting serial delivery; bitmap bit i acknowledges buffered
sequence base+i. Packets are buffered and delivered in order without duplicates.
ACKs ride on outgoing messages; standalone ACKs are sent after four arrivals or
a 32 ms delay. Only missing packets retransmit, with an RTT-derived timeout
clamped to 100–250 ms. Sequence exhaustion closes the call.

Command mode or a blocked host holds received data in the bounded window.
Acknowledgment means accepted into volatile buffering, not consumed by the
host application; power failure can lose buffered data. Software flow control
reserves UART bytes 0x11/0x13 and requires compatible escaping for arbitrary
binary protocols. Flow control is off by default. UART overrun fails the call.

After two seconds without authenticated core contact, radios scan with unequal
caller/answerer dwell times. Contact triggers retraining while retaining stream
queues and sequence state. Idle receivers remain on calling channel 25. Partial handshakes
anchor the channel unless repeated transmit failures require recovery.

## Coordinated energy surveys

SURVEY uses the core sequence as a nonzero survey ID and four payload bytes:
phase, channel-list index, little-endian 16-bit value. Phases are REQUEST=0,
READY=1, COMMIT=2, REPORT=3, QUERY=4, MOVE=5, MOVE_ACK=6. The established
session authenticates these messages and rejects transport replays.

The caller requests a slot; after READY it repeats COMMIT with a decreasing
remaining delay (initially 150 ms). Both endpoints stop ordinary transmissions,
retune at the scheduled time, measure eight milliseconds of energy after a
40 ms guard, and return at slot start +180 ms. REPORT carries signed dBm in
its low byte; 127 means unavailable. The caller records the worse endpoint's
value. Measurements expire after 65 seconds. An initial five-channel sweep
is followed by a candidate every ten seconds; a full refresh takes about 50 s.

A move requires fresh current/candidate measurements and improvement greater
than 6 dB. MOVE/MOVE_ACK schedule a retune with 200 ms guard, then retrain while
preserving stream sequence state. Control messages retry; survey state times
out after 2.5 s and returns to its origin channel. Lost move acknowledgments
can temporarily separate the radios; ordinary asymmetric channel recovery
reunites them. Scan timing is relative and guarded, not clock synchronization.
`AT+SURVEY?` combined scores/counts are maintained by the caller; both endpoints
count moves. Channel-change counters include temporary measurement retunes.


## Transmit-power feedback

POWER is an optional encrypted version-5 core message with exactly four payload
bytes: format version (1), last received RSSI + 128 (0 means unknown/−128),
retry/silence pressure (0/1), and sender's nominal applied TX power + 40.
The core sequence field is zero; normal cumulative/selective acknowledgements
remain in its header. Existing v5 peers that do not recognize POWER ignore it.
It must match the active peer and call session before it can affect power.
Feedback expires after six seconds. A peer can report quality but cannot set
or exceed the local power ceiling. Fixed mode ignores adjustment requests.
The 2-second reports and 4-second increase cooldown run outside survey slots;
retry pressure with fresh RSSI above −60 dBm requests a survey without raising
power, unless the peer has gone silent. Initial call and Noise-handshake
timeouts can raise power before authenticated quality feedback is available.
Decreases require 30 seconds of clean traffic, >=1024 acknowledged bytes per
observation and reported RSSI >−70 dBm. RSSI alone never triggers a decrease.

Saved profile format 2 retains the 40-byte record and CRC over bytes 0–35.
Byte 28 is automatic (0/1), byte 29 is configured power/ceiling + 20, bytes
30–35 remain zero. Format 1 loads with automatic control capped at +5 dBm.
The instantaneous adapted power is not saved: automatic mode starts at the
lower of +5 dBm and the configured ceiling.
