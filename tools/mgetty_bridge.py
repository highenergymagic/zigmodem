#!/usr/bin/env python3
"""Linux mgetty bridge for reset-wired SONOFF USB UARTs; see docs/mgetty.md."""
import argparse
import errno
import os, pty, select, signal, subprocess, time
import serial

def pump(terminal,uart,alive,carrier_check=False):
    # Bounded queues apply backpressure to the PTY, without blocking RX
    # while the UART is paused by XOFF. Every partial write is retained.
    cap=16384
    fds=(terminal,uart)
    pending={terminal:bytearray(),uart:bytearray()}
    for fd in fds: os.set_blocking(fd,False)
    tail=b''
    marker=b'\r\nNO CARRIER\r\n'
    while alive():
        readers=[fd for fd in fds if len(pending[uart if fd==terminal else terminal])<cap]
        writers=[fd for fd in fds if pending[fd]]
        readable,writable,_=select.select(readers,writers,[],0.2)
        for fd in writable:
            try: n=os.write(fd,pending[fd])
            except BlockingIOError: continue
            except OSError as exc:
                if exc.errno in (errno.EIO,errno.EPIPE): return
                raise
            if n: del pending[fd][:n]
        for fd in readable:
            target=uart if fd==terminal else terminal
            try: data=os.read(fd,min(4096,cap-len(pending[target])))
            except BlockingIOError: continue
            except OSError as exc:
                if exc.errno == errno.EIO and fd == terminal: return
                raise
            if not data: return
            if fd==uart and carrier_check:
                joined=tail+data
                if marker in joined: return
                tail=joined[-(len(marker)-1):]
            pending[target].extend(data)

def hangup(s):
    """Escape and ATH while the UART remains open; allow CLOSE retransmissions."""
    deadline=time.monotonic()+5
    while s.out_waiting:
        if time.monotonic()>deadline:
            raise TimeoutError('UART output remained blocked during hangup')
        time.sleep(0.05)
    # This requires the default S2=43, S12=50, Q0 and V1 settings.
    time.sleep(1.2)
    s.write(b'+++')
    time.sleep(1.2)
    s.write(b'\r')  # clears a partial command if already offline
    time.sleep(0.2)
    s.reset_input_buffer()
    s.write(b'ATH\r')
    response=bytearray()
    deadline=time.monotonic()+5
    while b'\r\nOK\r\n' not in response:
        if time.monotonic()>deadline:
            raise TimeoutError('No acknowledgment of ATH')
        response.extend(s.read(4096))
        if len(response)>8192: del response[:-8192]
        time.sleep(0.02)
    # ATH acknowledges locally before the peer has acknowledged CLOSE.
    # Closing/resetting the USB UART immediately can strand the peer online.
    time.sleep(3.2)
    s.reset_input_buffer()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port',help='physical USB UART, preferably /dev/serial/by-id/...')
    parser.add_argument('--device',default='/dev/zigmodem',help='new PTY symlink used by mgetty')
    parser.add_argument('--mgetty',default='/usr/sbin/mgetty')
    args=parser.parse_args()
    name=os.path.abspath(args.device)
    if os.path.dirname(name)!='/dev' or name==os.path.realpath(args.port):
        parser.error('--device must be a separate name directly under /dev')
    if os.path.lexists(name):
        parser.error('--device already exists; remove a stale bridge symlink explicitly')
    s=serial.Serial(port=None,baudrate=115200,timeout=0,write_timeout=5,xonxoff=True,exclusive=True)
    s.dtr=s.rts=False
    s.port=args.port
    s.open()
    stopping=False
    def stop(signum,frame):
        nonlocal stopping
        stopping=True
    signal.signal(signal.SIGTERM,stop)
    signal.signal(signal.SIGINT,stop)
    try:
        hangup(s)
        while not stopping:
            master,slave=pty.openpty()
            os.symlink(os.ttyname(slave),name)
            child=None
            try:
                child=subprocess.Popen([args.mgetty,'-D','-s','115200',os.path.basename(name)],start_new_session=True)
                pump(master,s.fileno(),lambda: not stopping and child.poll() is None,carrier_check=True)
            finally:
                if child is not None and child.poll() is None:
                    try: os.killpg(child.pid,signal.SIGTERM)
                    except ProcessLookupError: pass
                    try: child.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        try: os.killpg(child.pid,signal.SIGKILL)
                        except ProcessLookupError: pass
                        child.wait()
                os.close(master); os.close(slave)
                if os.path.islink(name): os.unlink(name)
                hangup(s)
    finally:
        s.close()

if __name__ == "__main__":
    main()
