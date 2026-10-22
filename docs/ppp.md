# PPP over Zigmodem

Qualified on the P/E pair with Linux pppd/chat 2.5.2, 115200 UART, default
adaptive 300–28800 radio policy, encryption, compression and surveys enabled.
LCP and IPCP negotiated successfully. Ten pings had no loss; a 32768-byte
incompressible TCP echo matched SHA256 in both directions in 36.84 seconds.
SSH carried orchestration only; all IP payload crossed the dongle radios.

## Serial wiring and flow control

Use `tools/ppp_serial.py` as pppd's `pty` transport. It opens the real USB UART
with DTR/RTS deasserted because those pins control reset/bootloader on these
boards. This bridge does not emulate DCD, RI or hardware hangup. Install
pyserial in the Python environment used by the bridge, and install ppp/chat.
Only one process may open each physical serial port.

Chat enables AT&K4. PPP must escape XON/XOFF (hex 11/13) in both directions:
`asyncmap a0000` requests this from the peer; `escape 11,13,2b` also forces it
locally. The example disables guarded escape with ATS2=128 for binary PPP.
No PPP password is needed for this isolated test: modem encryption and identity
pins remain active, but pins are not a caller access-control list. Configure
PPP authentication separately before exposing any wider network service.

## Reproduce in isolated Linux network namespaces

On each host, create a test namespace as root:

```sh
ip netns add zigmodem-test
```

Save this as `/absolute/path/answer.chat` on the answering host:

```text
ABORT ERROR
ABORT 'NO CARRIER'
ABORT BUSY
TIMEOUT 90
'' ATZ
OK ATE0
OK AT&K4
OK ATS2=128
OK ATS0=1
CONNECT ''
```

For the caller, save `dial.chat` with `ATS0=1` replaced by `ATDT1` (or the
remote ten-digit number). Slot 1 must already contain the intended peer.
Set verified identity pins beforehand when first-contact authentication matters.

Save a pppd options file on each host, replacing the absolute Python, repository,
chat and log paths, and the physical port. The example is the calling side;
on the answerer reverse the address pair and use `answer.chat`.

```text
nodetach
local
noauth
nodefaultroute
noproxyarp
noipv6
noccp
mtu 512
mru 512
asyncmap a0000
escape 11,13,2b
lcp-echo-interval 10
lcp-echo-failure 6
ip-up-script /bin/true
ip-down-script /bin/true
ip-pre-up-script /bin/true
192.0.2.1:192.0.2.2
pty "/absolute/python /absolute/repo/tools/ppp_serial.py /dev/ttyUSB1"
connect "/usr/sbin/chat -v -f /absolute/path/dial.chat"
logfile /absolute/path/ppp.log
debug
```

Run the answerer first, then the caller, as root on their respective hosts:

```sh
ip netns exec zigmodem-test pppd file /absolute/path/ppp.options
```

The namespaces prevent test routes from changing the hosts' networking.
After both logs report local/remote IP addresses, run on the caller:

```sh
ip netns exec zigmodem-test ping -c 10 -W 5 192.0.2.2
```

For binary TCP verification, run the listener on the answerer, then the
client on the caller, using the repository's test script:

```sh
ip netns exec zigmodem-test python3 tools/ppp_payload_test.py listen 192.0.2.2
ip netns exec zigmodem-test python3 tools/ppp_payload_test.py connect 192.0.2.2
```

Stop the two test pppd processes, then delete each test namespace with
`ip netns delete zigmodem-test`. Reset/replug both dongles afterward to end
the radio call and restore ordinary AT settings. Closing a PTY does not hang
up the firmware through DTR, and ATS2=128 intentionally disables +++.
This recipe creates no default route, NAT, forwarding or Internet access.
