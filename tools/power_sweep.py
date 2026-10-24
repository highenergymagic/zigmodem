#!/usr/bin/env python3
"""Controlled two-host RF power sweep. Stop getty/terminal owners first.

Copy this script to the peer. SSH carries control and hashes only; payload uses RF.
Example: python tools/power_sweep.py COM3 --peer freya@carbon
  --peer-command 'doas -n /home/freya/zigmodem-work/venv/bin/python -u
  /home/freya/zigmodem-work/tools/power_sweep.py /dev/ttyUSB1 --endpoint'
  --number 3793723694
Restores saved ATZ profiles on exit. Does not start/stop system services.
"""
import argparse
import hashlib
import json
import subprocess
import sys
import time
import serial


def payload(index, size):
    return hashlib.shake_256(f'power-sweep-{index}'.encode()).digest(size).replace(b'\x11', b'\x12').replace(b'\x13', b'\x14')


class Modem:
    def __init__(self, port):
        self.s = serial.Serial(port=None, baudrate=115200, timeout=.1, write_timeout=120, xonxoff=True)
        self.s.dtr = self.s.rts = False
        self.s.port = port
        self.s.open()
        self.online = False
        time.sleep(.6)
        self.s.reset_input_buffer()

    def wait(self, token, timeout=90, allow_carrier=False):
        out = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            out.extend(self.s.read(1))
            if token in out:
                return out.decode('ascii', errors='replace')
            if b'\r\nERROR\r\n' in out or (not allow_carrier and b'\r\nNO CARRIER\r\n' in out):
                raise RuntimeError(repr(out))
        raise TimeoutError(repr(out))

    def cmd(self, text, allow_carrier=False):
        self.s.write(text.encode() + b'\r')
        return self.wait(b'\r\nOK\r\n', 10, allow_carrier=allow_carrier)

    def setup(self):
        for c in ['ATZ', 'AT+COMPAT=NATIVE', 'ATE0Q0V1', 'AT&K4', 'ATS0=0', 'AT+MS=1,300,28800']:
            self.cmd(c)

    def connect(self):
        result = self.wait(b'\r\nCONNECT') + self.wait(b'\r\n', 10)
        self.online = True
        return result

    def escape(self):
        time.sleep(1.25)
        self.s.write(b'+++')
        result = self.wait(b'\r\nOK\r\n', 10)
        self.online = False
        return result

    def resume(self):
        self.s.write(b'ATO\r')
        return self.connect()

    def stats(self):
        return {c: self.cmd(c) for c in ['AT+POWER?', 'AT+STATS?', 'AT+LINK?', 'AT+SURVEY?']}

    def write(self, index, size):
        data = payload(index, size)
        for offset in range(0, size, 512):
            self.s.write(data[offset:offset + 512])

    def read(self, index, size):
        data = bytearray()
        deadline = time.monotonic() + 600
        while len(data) < size and time.monotonic() < deadline:
            data.extend(self.s.read(min(4096, size - len(data))))
        if bytes(data) != payload(index, size):
            raise RuntimeError(f'payload mismatch: {len(data)}/{size}')
        return hashlib.sha256(data).hexdigest()

    def close(self):
        try:
            if self.online:
                self.escape()
            # Remote hangup may already have queued NO CARRIER; consume it.
            time.sleep(.2)
            self.s.reset_input_buffer()
            self.cmd('ATH', allow_carrier=True)
            time.sleep(3.5)
            self.s.reset_input_buffer()
            self.cmd('ATZ')
        finally:
            self.s.close()


def endpoint(m):
    for line in sys.stdin:
        try:
            req = json.loads(line)
            op = req.pop('op')
            if op == 'answer':
                m.wait(b'\r\nRING\r\n', 120)
                m.s.write(b'ATA\r')
                result = m.connect()
            elif op == 'finish':
                print(json.dumps({'result': 'finished'}), flush=True)
                return
            else:
                result = getattr(m, op)(**req)
            print(json.dumps({'result': result}), flush=True)
        except Exception as exc:
            print(json.dumps({'error': repr(exc)}), flush=True)
            return


def controller(m, args):
    remote = subprocess.Popen(['ssh', args.peer, args.peer_command], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    def send(op, **kw):
        remote.stdin.write(json.dumps(dict(op=op, **kw)) + '\n')
        remote.stdin.flush()
    def receive():
        line = remote.stdout.readline()
        if not line:
            raise RuntimeError('peer endpoint ended')
        reply = json.loads(line)
        if 'error' in reply:
            raise RuntimeError(reply['error'])
        return reply['result']
    def rpc(op, **kw):
        send(op, **kw)
        return receive()
    def log(event, **kw):
        print(json.dumps(dict(event=event, **kw)), flush=True)
    try:
        m.setup()
        rpc('setup')
        m.cmd('AT+SCAN=0')
        rpc('cmd', text='AT+SCAN=0')
        send('answer')
        m.s.write(('ATDT' + args.number + '\r').encode())
        log('connected', local=m.connect(), peer=receive())
        m.escape()
        rpc('escape')
        stages = ([] if args.auto_only else [('FIXED', p) for p in [5, 20, 0, -10, -20]]) + [('AUTO', 20)]
        index = 0
        for mode, dbm in stages:
            text = f'AT+POWER={mode},{dbm}'
            m.cmd(text)
            rpc('cmd', text=text)
            if mode == 'AUTO':
                m.cmd('AT+SCAN=1')
                rpc('cmd', text='AT+SCAN=1')
            log('stage', mode=mode, dbm=dbm, local=m.stats(), peer=rpc('stats'))
            # Longer blocks allow automatic power reductions to observe steady traffic.
            size = 131072 if mode == 'AUTO' else 16384
            for direction in ['out', 'in']:
                rpc('resume')
                m.resume()
                began = time.monotonic()
                if direction == 'out':
                    send('read', index=index, size=size)
                    m.write(index, size)
                    digest = receive()
                else:
                    send('write', index=index, size=size)
                    digest = m.read(index, size)
                    receive()
                elapsed = time.monotonic() - began
                m.escape()
                rpc('escape')
                log('verified', mode=mode, dbm=dbm, direction=direction, bytes=size,
                    seconds=elapsed, kbps=size*8/elapsed/1000, sha256=digest,
                    local=m.stats(), peer=rpc('stats'))
                index += 1
        log('DONE', blocks=index)
        rpc('finish')
    finally:
        remote.stdin.close()
        try:
            remote.wait(timeout=20)
        except subprocess.TimeoutExpired:
            remote.terminate()
            remote.wait(timeout=10)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('port')
    p.add_argument('--endpoint', action='store_true')
    p.add_argument('--peer')
    p.add_argument('--peer-command')
    p.add_argument('--number')
    p.add_argument('--auto-only', action='store_true', help='only run the longer automatic-control stage')
    args = p.parse_args()
    if not args.endpoint and not all([args.peer, args.peer_command, args.number]):
        p.error('controller requires --peer, --peer-command and --number')
    m = Modem(args.port)
    try:
        endpoint(m) if args.endpoint else controller(m, args)
    finally:
        m.close()


if __name__ == '__main__':
    main()
