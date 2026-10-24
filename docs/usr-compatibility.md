# Sportster-style data commands (v0.5)

Zigmodem implements a selectable USRobotics Sportster-style **data command
subset**. It is not a drop-in Sportster 56K, a fax modem, or a certified Windows
modem. Identification deliberately says Zigmodem. The tested rating remains
28.8k; selecting a 56k pacing profile does not establish 56k throughput.

The command reference used is USRobotics' [external modem technical
reference](https://support.usr.com/support/5686e/5686e-ug/tech-ref.html).
Different Sportster revisions have different commands and result tables.

## Select and save a personality

Existing terminal setups default to `NATIVE`, where `&K4` means software flow
control. USR uses `&K` for compression, so the two interpretations must be
selected explicitly. Send each of these lines separately:

```text
AT+COMPAT=USR
AT&F2E0V1Q0&A3
AT&W1
ATY1
```

This selects software flow control in both directions, extended reports,
saves profile 1, and selects it for boot. Configure the host for XON/XOFF.
`AT+COMPAT?` reports the current personality. To restore a native terminal
profile and select it at boot:

```text
AT+COMPAT=NATIVE
AT&F2E0V1Q0&W0&Y0
```

There are two saved profiles, each including personality, rate range,
compression, survey policy, flow control, result formatting and S-registers.
`AT&W0`/`AT&W1` writes and verifies them. The omitted slot is 0. A failed
write returns ERROR. CRC-protected profiles and the boot selection live in
the same preserved NVS partition as identity/pins, under separate IDs
0x10–0x12. They survive application upgrades and power cycles. Factory reset
does not erase saved profiles, identity, or pins. An absent/invalid selected
profile falls back to factory settings. Never mass-erase during an upgrade.

| Function | Native personality | USR personality |
| --- | --- | --- |
| Select boot profile | `&Y0`, `&Y1` | `Y0`, `Y1` |
| Reset to boot selection | `Z`, `Z0` | `Z`, `Z0` |
| Load profile 0 | Select `&Y0`, then `Z` | `Z1` |
| Load profile 1 | `Z1` | `Z2` |
| Factory settings, flow off | `&F`, `&F0` | `&F`, `&F0`, `Z3`; `Y2` selects at boot |
| Factory software flow | `&F2` | `&F2`, `Z5`; `Y4` selects at boot |
| Current and saved settings | `&V` | `&V` |

Loading a profile also loads its personality. `&F` preserves the current
personality. Hardware-flow factory selectors `&F1`, USR `Z4`/`Y3` return ERROR.

## Inquiries

| Command | Actual response |
| --- | --- |
| `I`, `I0` | Zigmodem product code 2880 |
| `I1` | Decimal CRC32 of the running firmware ROM region |
| `I2` | Nondestructive 64-byte scratch RAM pattern test, then OK; not a whole-RAM test |
| `I3` | Zigmodem firmware version and rating |
| `I4` | Active settings |
| `I5` | Both saved profiles; marks the selected profile when applicable |
| `I6` | Current link and cumulative counters since boot |
| `I7` | Actual hardware-independent capabilities and limitations |
| `I9` | Informational HGM2880 identifier; does not change USB descriptors or implement serial PnP |
| `I11` | Extended link, authentication, compression and survey counters |

Reserved/undefined inquiries, including I8 and I10, return ERROR. I6/I11 are
not a historical last-call record. I1 does not checksum private NVS or certify
the image against a known-good value. I2 has its own OK test response followed
by the command's final OK.

## Working data controls

| Commands | Implementation |
| --- | --- |
| `E0/1`, `V0/1`, `Q0/1` | Echo, numeric/text results, suppress results |
| `A/` | Repeat last command immediately, without a carriage return |
| `X0` | Bare CONNECT; X1–X4 include rate; USR X0–X2 map BUSY/NO ANSWER to NO CARRIER |
| USR `&A0` / `&A1`–`&A3` | Basic / extended radio connection reports |
| `W0` / `W1`–`W2` | Same basic / extended report selection |
| USR `&H0/2` | Disable/enable modem-to-host XON/XOFF requests |
| USR `&I0/2` | Disable/enable honoring host XON/XOFF |
| USR `&K0` / `&K1`–`&K3`, `%C0/1` | Disable/enable outgoing Heatshrink compression |
| USR `&M4/5` | Reliable selective-repeat link; plaintext/unreliable mode is unavailable |
| USR `&B1` | Fixed 115200 host UART |
| USR `&Nn`, `&Un` | Maximum/minimum rate codes, below |
| `S0`, `S1?` | Auto-answer ring threshold / current ring count |
| `S2`, `S3`, `S4`, `S5` | Escape, command terminator, response linefeed, backspace characters |
| `S7`, `S12` | Dial timeout / escape guard in 20ms units |
| `S13=0/128` | Guarded escape enters command mode / hangs up |
| `S19=0` / `S19=n` | Disable inactivity timeout / hang up after n minutes without user data |
| `D`, `DT`, `DP`, `A`, `H`, `O0/1` | Dial, answer, hangup, resume/retrain |

S-registers support `?` and `=n`; S1 is
read-only. S13 supports only bit 7. Firmware keepalives do not reset S19.
XON/XOFF consumes bytes 0x11/0x13; binary protocols must escape them.

USR rate codes 1–16 mean 300,1200,2400,4800,7200,9600,12000,14400,16800,
19200,21600,24000,26400,28800,31200,33600; code 38 means 56000. Other nonzero
codes return ERROR. N=U=0 restores adaptive 300–28800. N>0/U=0 requests a
fixed rate; N>0/U>0 sets an adaptive range. U cannot exceed N. With N=0 the
ceiling is 28800. Changes apply to the next call. `+MS` offers explicit rates
and resets the stored USR rate-code selectors.

Extended reports identify the real protocols:

```text
CARRIER 28800
PROTOCOL: ZIGMODEM-ARQ
COMPRESSION: HEATSHRINK
CONNECT 28800
```

COMPRESSION describes the local outgoing policy, not a negotiated V.42bis
mode. Unlike a Sportster, A1/A2/A3 currently share this report; W1/W2 also
share it. Numeric mode emits the corresponding basic Sportster rate code
(e.g. 107 for 28800); it does not encode ARQ suffix variants. No MNP,
LAPM, V.42bis, V.90 or V.92 capability is claimed. Rate is directional payload
pacing over the fixed 250 kbit/s radio PHY, not a telephone carrier or UART rate.

## Windows driver boundary

Stock Sportster INF initialization is **not supported unchanged**. For example,
Windows' `mdmusrsp.inf` uses `&F1 &C1 &D2 &B1 &A3` and hardware flow settings.
These SONOFF USB bridges wire DTR/RTS to reset/boot and do not expose useful
carrier detect or hardware flow control. Sending DTR may reset the firmware.
The bridge's USB identity also remains Silicon Labs/SONOFF.

Hardware controls (`&C1`, `&D2`, `&H1`, `&R2`, `&S`), telephone signaling,
fax/voice, unimplemented S-registers, break controls, and unsupported dial
modifiers return ERROR. `+FCLASS` supports data class 0 only. Pin-book speed
dialing is `DT1`–`DT16`, not USR `&Z` storage. All undefined commands return
ERROR; this is a compatibility subset rather than an enumeration of every
historical Sportster feature.

A true Windows modem-device interface would additionally require a host
virtual serial/modem driver that supplies carrier/DTR semantics and translates
them into reliable control messages, or different hardware. The Linux
[mgetty bridge](mgetty.md) already provides terminal-session cleanup in software.
PuTTY can use the physical COM port directly with the settings above.

## Qualification

`tests/test_profiles.c` checks both personalities, profile CRC rejection and
write failures, boot/reset selection, inquiries, rate bounds, numeric and
extended results, custom delimiters, repeat, inactivity and escape hangup.
Both native and microcontroller Curve25519 suites pass ASan/UBSan.

For a dedicated test pair, stop other serial users and run
`python tools/qualify_profiles.py PORT prepare`, reflash the application, then
run the same command with `verify`. This overwrites both saved profiles and
restores native profile 0 afterward. The P/E hardware pair passed this check
with unchanged phone numbers and pins. A 16 KiB duplex radio test passed with
zero UART overruns, extended reports, manual retraining and a channel move.
