# Zigmodem encrypted wire protocol v2

This is a custom application protocol over the 250 kbit/s 2.4 GHz IEEE
802.15.4 PHY, not a Zigbee profile. Peers discover/recover across channels
25,15,20,26,11 and use PAN `0x5a4d`.
One shared radio channel is half duplex; calls provide simultaneous logical
streams using alternating packets.

## MAC and secure envelope

The MAC envelope has frame control `0xc841` (data, PAN compression, short
broadcast destination, extended source), an 8-bit MAC sequence, 16-bit PAN,
destination `0xffff`, and 64-bit source. Its length is 15 bytes. The radio
hardware generates/checks the FCS. No MAC ACK is requested; application ACKs
provide reliability. No FCS bytes appear in the modem core's input.

All multibyte protocol integers are little endian. MAC routing uses the first
64 fingerprint bits. The dialed identity is all 128 fingerprint bits; the
caller checks it against the authenticated Noise remote public key.

Each radio payload begins with the following secure envelope:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 2 | ASCII `ZS` |
| 2 | 1 | Version, 2 |
| 3 | 1 | HS1=1, HS2=2, HS3=3, encrypted DATA=4, encrypted READY=5 |
| 4 | 8 | Source routing prefix |
| 12 | 8 | Destination routing prefix |
| 20 | 8 | Call session ID |
| 28 | 8 | Directional AEAD counter; zero for handshake messages |
| 36 | 4 | Reserved, zero |
| 40 | 1 | Fragment index |
| 41 | 1 | Fragment count |
| 42 | 2 | Complete record length |
| 44 | 1–64 | Fragment bytes |

Records are bounded to 160 bytes. Maximum radio payload is 108 bytes, or
125 bytes with MAC header and FCS. Reassembly lengths and fragment offsets
are checked before copying. Encryption authenticates the first 40 envelope
bytes; fragment tampering can only cause reassembly failure or tag rejection.

Noise_XX_25519_ChaChaPoly_BLAKE2s uses fresh ephemeral keys per call and the
persistent identity key. The prologue is `ZM2\0`, caller routing prefix,
callee routing prefix, and call session ID. The caller verifies the callee's
full fingerprint after HS2, before sending HS3. The callee verifies possession
of the caller's key and derives its identity; it does not impose a caller ACL.
READY is encrypted with the responder's split transport key. Only then does
the initiator send the encrypted CALL. Cached handshake messages can be
retransmitted without advancing Noise state. They expire after 15 seconds.

Transport directions have independent keys and monotonically increasing
64-bit counters. A 64-record replay window is advanced only after successful
authentication; a forged high counter cannot poison it. Application-level
retries receive fresh AEAD counters. Sessions cannot continue across reset.

Encrypted DATA plaintext begins with a compression flag and a two-byte
uncompressed length, followed by the modem message. Heatshrink uses window
bits=8, lookahead bits=4 and a new dictionary for every record. Compressed
records are used only if smaller; decompression is bounded to one modem frame.
READY has empty plaintext. All encrypted records include a 16-byte tag.

## Inner modem messages

The existing core message format is carried only inside encrypted DATA in
firmware. Its version byte remains 1; it is not accepted as a plaintext radio
packet. The legacy PTY simulator still tests the unencrypted core in isolation.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 2 | ASCII `ZM` |
| 2 | 1 | Version, 1 |
| 3 | 1 | Message type |
| 4 | 8 | Source number |
| 12 | 8 | Destination number |
| 20 | 8 | Call session ID |
| 28 | 4 | Data/ACK sequence number |
| 32 | 0–64 | Data payload |

Control messages have exactly 32 bytes except CALL/ACCEPT, whose three-byte
capabilities payload contains version=2, maximum profile and minimum profile.
DATA contains 1–64 bytes; PROBE contains a 64-byte training pattern.

Types: CALL=1, ACCEPT=2, CONFIRM=3, BUSY=4, DATA=5, ACK=6, PING=7,
PONG=8, CLOSE=9, CLOSED=10, WAIT=11, PROBE=12, PROBE_ACK=13.

## Calls

The caller chooses a new nonzero 64-bit session ID from a boot-time random
seed plus a per-call counter. A new boot receives a new hardware-random seed.
Noise authenticates the handshake binding to this session ID.

CALL repeats every 500 ms until answered or the configured timeout expires.
The callee enters RINGING, reports RING every 2 seconds, and sends ACCEPT
when answered. ACCEPT repeats every 500 ms until CONFIRM arrives. The caller
sends CONFIRM and starts training on ACCEPT. CONNECT follows successful
training. Duplicate ACCEPTs receive a new
CONFIRM. DATA or PING from the caller also confirms the callee's connection
if CONFIRM was lost. An unanswered inbound call expires after 60 seconds.
The core supports BUSY; the single-session encrypted firmware ignores new
handshakes while occupied, so an additional caller normally times out.

CLOSE is retried every 300 ms for up to 3 seconds until CLOSED is received.
Duplicate CLOSEs for the last session are acknowledged even after becoming
idle. Connected peers exchange PING/PONG each second and abandon a silent
peer after 30 seconds. After two seconds of silence, radios scan with different
caller/answerer dwell times. Authenticated contact stops scanning and triggers
training without resetting byte sequence state. Idle receivers favor channel
25. Handshake progress anchors a channel until it completes or times out.

## Training and adaptation

Profiles 0..4 target 300,1200,2400,4800,9600 useful payload bits per second.
The highest mutually permitted profile is tried first, using eight full-size
probes paced at that rate. Fewer than seven timely replies causes fallback
when adaptation is enabled. At the minimum profile at least two replies are
required. Training is bounded to 60 seconds. These are heuristic profiles,
not a measured guaranteed throughput or a change in PHY modulation.

During data transfer, retry counts reduce profiles; 32 clean acknowledged
frames permit an increase. Repeated failed sends can initiate channel recovery
even when some inbound frames are still arriving. `ATO1` requests retraining.

## Stream

Each direction has an independent 32-bit sequence starting at zero and one
outstanding DATA frame. An unacknowledged frame is retried after a jittered
150–300 ms delay to avoid synchronized collisions between the two peers.
The receiver delivers only its next expected sequence. A duplicate of the
previous sequence is acknowledged without redelivery. Other sequences are
discarded. The connection closes before sequence reuse.

ACK is sent only when the complete payload fits in the receiving serial
queue. Command mode holds new incoming data without acknowledgment, while
heartbeats keep the session alive. A dropped ACK cannot duplicate serial
output. The portable input API returns the consumed byte count so a host
transport can retain unsent input. A physical UART overrun is unrecoverable
and explicitly fails the call.

This delivers bytes while the endpoint remains running. It cannot guarantee
exactly-once delivery across power failure or guarantee that an attached
application consumed bytes already accepted by the UART.
