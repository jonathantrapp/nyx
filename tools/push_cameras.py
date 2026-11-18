"""Send cameras/cameras.bin to the unit over USB serial (stored as /cameras.bin on its flash).

    python tools/push_cameras.py COM6
    python tools/push_cameras.py COM6 path/to/other.bin

Needs pyserial. Sends 1 KB chunks, waiting for the unit's ACK after each, then checks the
CRC32 the unit computed. Sessions on the unit are never touched. If the unit says it lacks
free space, pull the sessions you want (tools/pull_session.py) and `delete` old ones first.
"""
import sys
import time
import zlib
from pathlib import Path

import serial  # pyserial

ROOT = Path(__file__).resolve().parent.parent


def read_line(port, timeout):
    deadline = time.time() + timeout
    buf = b''
    while time.time() < deadline:
        c = port.read(1)
        if c == b'\n':
            return buf.decode('utf-8', 'replace').strip()
        if c:
            buf += c
    return None


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / 'cameras/cameras.bin'
    data = path.read_bytes()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = sys.argv[1], 921600, 0.2
    port.dtr = port.rts = False          # don't toggle the reset lines
    port.open()
    with port:
        port.reset_input_buffer()
        port.write(f'camput {len(data)} {crc:08x}\n'.encode())
        chunk = None
        while True:
            line = read_line(port, 8)
            if line is None:
                sys.exit('no answer from the unit (is it running firmware 0.4.0 or later?)')
            if line.startswith('CAMPUT READY'):
                chunk = int(line.split()[2])
                break
            if line.startswith('CAMPUT FAIL') or line.startswith('usage'):
                sys.exit(line)
        sent = 0
        started = time.time()
        while sent < len(data):
            piece = data[sent:sent + chunk]
            port.write(piece)
            sent += len(piece)
            while True:                     # wait for this chunk's ACK (skip any unrelated log lines)
                line = read_line(port, 6)
                if line is None:
                    sys.exit(f'timed out waiting for ACK at byte {sent}')
                if line.startswith('ACK '):
                    break
                if line.startswith('CAMPUT FAIL'):
                    sys.exit(line)
            if (sent // chunk) % 200 == 0:
                print(f'{sent * 100 // len(data)}%  ({sent}/{len(data)} bytes)', flush=True)
        while True:
            line = read_line(port, 10)
            if line is None:
                sys.exit('no final answer from the unit')
            if line.startswith('CAMPUT') or line.startswith('camera map'):
                print(line)
            if line.startswith('CAMPUT FAIL'):
                sys.exit(1)
            if line.startswith('camera map:'):
                break
    print(f'done in {time.time() - started:.0f} s')


if __name__ == '__main__':
    main()
