"""tft_shot.py - screenshot of what the unit's colour screen is drawing, as a PNG.

    python tools/tft_shot.py                       # -> captures/tft_shot.png (auto-finds the board)
    python tools/tft_shot.py out.png COM6          # explicit output / port

Sends `tft dump` over the board's native USB port; the firmware prints its frame buffer (the same
pixels it pushes to the panel) as base64, and this decodes it. Works even with no screen wired if
the driver is running ('tft on'), so it also checks the firmware's drawing without the hardware.
To look at a particular screen first:  tft demo camera 60   (then run this within 60 s).
Needs pyserial. No other dependencies.
"""
import base64
import re
import struct
import sys
import time
import zlib
from pathlib import Path

import serial
from serial.tools import list_ports


def find_port():
    for p in list_ports.comports():
        if p.vid == 0x303A:
            return p.device
    raise SystemExit('no ESP32-S3 on native USB (303A) found; pass the port as the 2nd argument')


def png_bytes(rgb, w, h):
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else 'captures/tft_shot.png')
    port = sys.argv[2] if len(sys.argv) > 2 else find_port()
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = False      # opening the port must not reset the board
    s.open()
    s.reset_input_buffer()
    s.write(b'tft dump\r\n')
    buf, t0 = bytearray(), time.time()
    while b'TFTDUMP END' not in buf and time.time() - t0 < 30:
        buf.extend(s.read(65536))
        if b'tft not running' in buf:
            raise SystemExit('the firmware says the screen driver is not running (try: tft on)')
    s.close()
    m = re.search(rb'TFTDUMP BEGIN (\d+) (\d+) rgb565le\r?\n(.*?)TFTDUMP END', bytes(buf), re.S)
    if not m:
        raise SystemExit('no frame in the reply (is this firmware 0.5.0 or newer?)')
    w, h = int(m.group(1)), int(m.group(2))
    data = base64.b64decode(b''.join(m.group(3).split()))
    if len(data) != w * h * 2:
        raise SystemExit(f'frame is {len(data)} bytes, expected {w * h * 2}')
    rgb = bytearray(w * h * 3)
    for i in range(w * h):
        v = data[2 * i] | (data[2 * i + 1] << 8)
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        rgb[3 * i] = (r << 3) | (r >> 2)
        rgb[3 * i + 1] = (g << 2) | (g >> 4)
        rgb[3 * i + 2] = (b << 3) | (b >> 2)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(png_bytes(bytes(rgb), w, h))
    print(f'{w}x{h} frame saved to {out}')


if __name__ == '__main__':
    main()
