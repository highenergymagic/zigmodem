from pathlib import Path
import importlib.util, socket, threading, hashlib, time
spec=importlib.util.spec_from_file_location('bridge',str(Path(__file__).resolve().parents[1]/'tools/mgetty_bridge.py'))
b=importlib.util.module_from_spec(spec); spec.loader.exec_module(b)
a,aa=socket.socketpair(); z,zz=socket.socketpair()
for s in (a,aa,z,zz): s.setsockopt(socket.SOL_SOCKET,socket.SO_SNDBUF,2048)
active=threading.Event(); active.set()
pump=threading.Thread(target=b.pump,args=(aa.fileno(),zz.fileno(),active.is_set),daemon=True);pump.start()
x=hashlib.shake_256(b'a').digest(262144); y=hashlib.shake_256(b'b').digest(262144)
errors=[]
def send(s,data):
    try: s.sendall(data)
    except Exception as e: errors.append(e)
def receive(s,want):
    try:
        s.settimeout(15); got=bytearray()
        while len(got)<len(want):
            p=s.recv(min(173,len(want)-len(got)))
            if not p: raise RuntimeError('EOF')
            got.extend(p)
        assert got==want
    except Exception as e: errors.append(e)
tx=[threading.Thread(target=send,args=(a,x)),threading.Thread(target=send,args=(z,y))]
for t in tx:t.start()
# Deliberately fill socket and bridge buffers before either host reads.
time.sleep(0.5)
rx=[threading.Thread(target=receive,args=(a,y)),threading.Thread(target=receive,args=(z,x))]
for t in rx:t.start()
for t in tx+rx:t.join(20)
assert not errors,errors
assert all(not t.is_alive() for t in tx+rx)
active.clear();pump.join(2)
print('PASS: 256 KiB each direction, bounded buffering, blocked hosts, partial writes')

# The hangup path must send ATH and leave time for its radio CLOSE exchange.
from unittest.mock import patch
class FakeSerial:
    out_waiting=0
    def __init__(self): self.commands=[]; self.answer=b''
    def write(self,data):
        self.commands.append(data)
        if data==b'ATH\r': self.answer=b'\r\nOK\r\n'
    def reset_input_buffer(self): self.answer=b''
    def read(self,n):
        result=self.answer; self.answer=b''; return result
fake=FakeSerial()
with patch.object(b.time,'sleep') as sleep:
    b.hangup(fake)
assert fake.commands==[b'+++',b'\r',b'ATH\r']
assert sleep.call_args_list[-1].args==(3.2,)
# A split NO CARRIER must terminate the relay without needing a hardware DCD.
a2,aa2=socket.socketpair();z2,zz2=socket.socketpair()
p=threading.Thread(target=b.pump,args=(aa2.fileno(),zz2.fileno(),lambda:True,True),daemon=True)
p.start();z2.sendall(b'\r\nNO CARR');time.sleep(.05);z2.sendall(b'IER\r\n')
p.join(2);assert not p.is_alive()
for s in (a,aa,z,zz,a2,aa2,z2,zz2):s.close()
print('PASS: acknowledged hangup with CLOSE grace period and fragmented carrier loss')
