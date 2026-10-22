# Zigmodem wire protocol v1

This is a custom application protocol over the 250 kbit/s 2.4 GHz IEEE
802.15.4 PHY, not a Zigbee profile. Peers use channel 25 and PAN `0x5a4d`.
One shared radio channel is half duplex; calls provide simultaneous logical
streams using alternating packets.

## Frame format

The MAC envelope has frame control `0xc841` (data, PAN compression, short
broadcast destination, extended source), an 8-bit MAC sequence, 16-bit PAN,
destination `0xffff`, and 64-bit source. Its length is 15 bytes. The radio
hardware generates/checks the FCS. No MAC ACK is requested; application ACKs
provide reliability. No FCS bytes appear in the modem core's input.

All multibyte protocol integers are little endian. The full source identity
in the MAC header must match the application header. This is validation, not
authentication: a nearby transmitter can forge it.

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

Control messages have exactly 32 bytes. DATA contains 1–64 payload bytes.
The largest on-air frame is 113 bytes including MAC header and FCS.

Types: CALL=1, ACCEPT=2, CONFIRM=3, BUSY=4, DATA=5, ACK=6, PING=7,
PONG=8, CLOSE=9, CLOSED=10.

## Calls

The caller chooses a new nonzero 64-bit session ID from a boot-time random
seed plus a per-call counter. A new boot receives a new hardware-random seed.
This prevents accidental reuse; it does not authenticate sessions.

CALL repeats every 500 ms until answered or the configured timeout expires.
The callee enters RINGING, reports RING every 2 seconds, and sends ACCEPT
when answered. ACCEPT repeats every 500 ms until CONFIRM arrives. The caller
sends CONFIRM and reports CONNECT on ACCEPT. Duplicate ACCEPTs receive a new
CONFIRM. DATA or PING from the caller also confirms the callee's connection
if CONFIRM was lost. An unanswered inbound call expires after 60 seconds.
Busy peers return BUSY; simultaneous cross-dialing returns BUSY to both.

CLOSE is retried every 300 ms for up to 3 seconds until CLOSED is received.
Duplicate CLOSEs for the last session are acknowledged even after becoming
idle. Connected peers exchange PING/PONG each second and abandon a silent
peer after 10 seconds.

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
