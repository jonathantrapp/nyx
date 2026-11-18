"""pull_session.py - copy one session off the board and check it arrived intact.

    python tools/pull_session.py s0027                    # -> captures/s0027.csv
    python tools/pull_session.py s0027 captures/x.csv

Checks the byte count and the CRC32 the firmware prints after the dump, and retries up
to 3 times. Needs pyserial and the board on its native USB port (303A).
"""
import re
import sys
import time
import zlib
from pathlib import Path

import serial
from serial.tools import list_ports


def pull_once(port, name):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = False
    s.open()
    s.reset_input_buffer()
    s.write(f"dump {name}\r\n".encode())
    buf, t0 = bytearray(), time.time()
    tail = re.compile(rb"=== CRC32 " + re.escape(name.encode()) + rb"\.csv [0-9a-f]{8} \w+ ===")
    while not tail.search(buf) and time.time() - t0 < 300:
        buf.extend(s.read(65536))
    s.close()
    n = re.escape(name.encode())
    m = re.search(rb"=== BEGIN " + n + rb"\.csv (\d+) ===\r?\n(.*)=== END " + n + rb"\.csv ===\r?\n"
                  rb"=== CRC32 " + n + rb"\.csv ([0-9a-f]{8}) (\w+) ===", buf, re.S)
    if not m:
        return None, f"incomplete ({len(buf)} bytes received)"
    size, body, crc, status = int(m.group(1)), m.group(2), int(m.group(3), 16), m.group(4).decode()
    if status != "ok":
        return None, "board aborted the dump (host stopped reading)"
    if len(body) != size:
        return None, f"size mismatch: expected {size}, got {len(body)}"
    if zlib.crc32(body) != crc:
        return None, "CRC mismatch"
    return body, f"{size:,} bytes, CRC ok"


def main():
    name = sys.argv[1].removesuffix(".csv")
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parent.parent / "captures" / f"{name}.csv"
    port = next((p.device for p in list_ports.comports() if p.vid == 0x303A), None)
    if not port:
        sys.exit("board not found on native USB (303A)")
    for attempt in range(1, 4):
        body, msg = pull_once(port, name)
        print(f"try {attempt}: {msg}")
        if body is not None:
            out.parent.mkdir(exist_ok=True)
            out.write_bytes(body)
            print(f"-> {out}")
            return
    sys.exit("failed 3 times")


if __name__ == "__main__":
    main()
