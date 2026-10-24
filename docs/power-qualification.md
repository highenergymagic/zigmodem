# Stationary power-control qualification

2026-10-25 (New Zealand): ZBDongle-E on Windows to ZBDongle-P on carbon,
in the existing nearby placement. All payloads were deterministic pseudorandom
bytes, excluding XON/XOFF, checked byte-for-byte at the receiving host. SSH
carried test control and hashes, never payload. UART: 115200, software flow
control; adaptive modem rate capped at 28800 bit/s.

The fixed-power sweep disabled channel surveys and kept one call alive. Each
setting transferred 16 KiB in each direction. These are sequential observations
under changing interference and inherited rate adaptation, not isolated fixed-rate
benchmarks or measurements of physical distance.

| Nominal TX power, both ends | E → P, kbit/s | P → E, kbit/s | RSSI at P / E, dBm |
| --- | ---: | ---: | --- |
| +5 dBm | 27.11 | 23.12 | −40 / −35 |
| +20 dBm | 26.48 | 24.39 | −26 / −19 |
| 0 dBm | 21.79 | 17.53 | −55 / −41 |
| −10 dBm | 15.37 | 9.94 | −63 / −50 |
| −20 dBm | 9.93 | 6.54 | −69 / −60 |

Every block passed. The initial sweep then transferred another 128 KiB each
way in automatic mode, for 416 KiB total and 189.38 seconds of active transfer.
Both overrun and radio-error counters remained zero. Its first controller
revision unnecessarily increased power during strong-signal interference.
This led to the strong-signal survey preference now in the controller.
The harness also initially treated an expected cleanup NO CARRIER as a failure;
that cleanup bug was fixed separately from the completed payload verification.

The final firmware's automatic-mode repeat transferred 128 KiB each way:
21.59 kbit/s E → P and 17.22 kbit/s P → E, including surveys and retraining.
All 256 KiB passed, both radios remained at +5 dBm with a +20 dBm ceiling,
and four coordinated channel moves occurred. Final rates were 26400 bit/s
on channel 20. Retries were 71 at E and 90 at P; overruns, radio errors and
power-driver errors were zero. The harness exited successfully, including
hangup and profile restoration. Both modems then saved AUTO,20, reloaded it
with ATZ, and successfully reconnected to carbon's mgetty login at CONNECT 28800.

This placement did not reach the loss-of-carrier threshold, even at −20 dBm.
No outdoor range claim follows from these tests. Power reductions and recovery
from a complete asymmetric signal deficit were exercised in deterministic core
tests; the physical automatic-mode repeat did not trigger a power reduction.
