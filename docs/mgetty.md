# Dial-in terminal login with mgetty

The Linux mgetty binary handles RING, ATA, CONNECT and the ordinary login
program. `tools/mgetty_bridge.py` provides a bounded PTY-to-USB bridge for
SONOFF boards whose DTR/RTS pins control reset/boot instead of carrier/flow.
No firmware update is needed for this bridge with Zigmodem v0.4.

## Install on the answering Linux host

Install the distribution's `mgetty` package and pyserial in a Python virtual
environment. Append [the port configuration](../examples/mgetty/mgetty.config)
to `/etc/mgetty/mgetty.config`. Keep the standard password-authenticated login
dispatcher; do not enable automatic shell login.

Copy [the systemd unit](../examples/mgetty/zigmodem-mgetty.service) to
`/etc/systemd/system/zigmodem-mgetty.service`, replacing its Python/repository
paths and dongle path. Prefer the stable `/dev/serial/by-id/` path. Then run:

```sh
sudo systemctl daemon-reload
sudo systemctl start zigmodem-mgetty
sudo systemctl status zigmodem-mgetty
sudo tail /var/log/mgetty/mg_zigmodem.log
```

Use `doas` instead of `sudo` where appropriate. Enable the service separately
if you want it started at boot. Stop it before flashing or running any other
serial tool. A leftover `/dev/zigmodem` symlink after an unclean shutdown must
be checked and removed while the service is stopped; the bridge refuses to
replace an existing device path.

The service needs privileges to run mgetty/login and create `/dev/zigmodem`.
Treat its source, Python environment and configuration as privileged files.
`ignore-carrier yes` is necessary on the PTY; the bridge supplies session
cleanup. The expected TIOCMGET/DSR warning comes from querying nonexistent
PTY modem pins. `fido-send-emsi no` disables the Fido handshake probe for this
ordinary terminal login. In mgetty prompts, `@` expands the hostname and
`\s` expands the operating-system name, not a space.

## PuTTY on the calling host

Select the dongle's COM port, 115200 baud, 8 data bits, one stop bit, no parity,
and **XON/XOFF** flow control. Leave local echo off/automatic. Send:

```text
ATZ
AT&K4
ATDT<answering modem's number>
```

After CONNECT, log in with the answering host's normal account credentials.
`ATZ` restores defaults, so enable AT&K4 after it. Flow control reserves
0x11/0x13 and is suitable for terminals; arbitrary binary transfers need an
escaping protocol such as the documented [PPP setup](ppp.md).

## Buffering and disconnects

Firmware has an 8192-byte serial RX buffer, a 2048-byte modem transmit queue,
and a 4096-byte serial TX buffer. XON/XOFF pauses the host before input buffers
fill. The bridge adds bounded 16384-byte queues in each direction. It handles
partial writes and continues receiving while the opposite direction is blocked.
Buffers absorb bursts; they cannot make sustained input above radio capacity
safe without flow control.

When login exits, the bridge waits for pending UART output, sends guarded
`+++`, sends `ATH`, verifies its OK, and keeps the UART open for the radio
CLOSE exchange. The caller should receive **NO CARRIER** within a few seconds.
Only then is the next mgetty session started. The physical UART stays open
across sessions, avoiding a USB control-line reset between logout and hangup.
Keep the default escape settings S2=43 and S12=50 for this service.

A received CRLF-NO CARRIER-CRLF terminates the current server session. Because
this is an in-band indication rather than hardware DCD, a caller sending that
exact text can also terminate their own session. This bridge is for terminal
login, not a transparent binary transport. Use the PPP bridge for PPP traffic.
Stopping the service also attempts the guarded hangup; USB removal/power loss
cannot guarantee an immediate notification to the peer, which instead times out.

Qualification: a 256 KiB simultaneous socket test with deliberately blocked
receivers verifies buffering/partial writes; a fragmented carrier-loss test and
hangup sequencing test run in CI. On the real Windows E/carbon P pair, ending
the mgetty session at its login prompt produced NO CARRIER and restored AT
command mode; redial succeeded, and caller-initiated hangup cleared the server.
This automated check does not enter a user's password. Interactive authenticated
login and shell logout were also exercised during initial setup.

## Modem controls beyond CONNECT

The default 28.8k maximum is a pacing policy; UART speed stays 115200 and actual
payload throughput is lower when both directions share airtime or scans/retries
occur. These controls implement real functions today:

| Control | Function |
| --- | --- |
| `AT&K4` | Local XON/XOFF flow control; match the host setting |
| `AT%C1` / `AT%C0` | Enable/disable outgoing Heatshrink compression; on by default |
| `AT+MS=1,300,28800` | Adaptive rate range, the default policy |
| `ATO1` | Retrain while retaining the call and buffered stream |
| `AT+LINK?` | Current directional rate, channel, retraining and RTT |
| `AT+SEC?` | Encryption/authentication counters and compression savings |
| `AT+STATS?` | Retransmissions, delivered bytes and UART overruns |
| `AT+SURVEY?` | Caller-side combined channel scores and coordinated moves |
| `AT+SCAN=0/1` | Caller-initiated proactive surveys off/on |

To query during a call, stop typing for a second, send `+++` without Enter,
wait another second for OK, issue queries, then `ATO` to return. `ATO1` instead
requests retraining. These are local modem commands, not shell commands.

Compression can increase useful throughput for repetitive text, but already
compressed/encrypted data usually gains nothing. Reliable selective-repeat
error correction and encryption are always active; they are not optional V.42,
MNP, V.42bis or V.44 implementations. Do not infer those standards from the
modem-like interface. There is no plaintext fallback.

Saved profiles (`AT&W`, `AT&Y`), a combined profile display (`AT&V`), and richer
CONNECT/compression/error-control reports are possible future firmware work.
They are not implemented or advertised as working compatibility stubs.
