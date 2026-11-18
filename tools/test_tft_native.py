"""Test the colour-screen code on the PC and render every screen to PNG files.

    python tools/test_tft_native.py            # tests + preview in tools/tft_preview/
    python tools/test_tft_native.py --open     # ...and open the contact sheet afterwards

Builds tools/tests/tft_native_test.cpp together with src/gfx.cpp and src/tft_screens.cpp (unchanged
firmware sources), plus the pieces of U8g2 it needs (the library the OLED already uses, found in
.pio/libdeps). The font decoder is compared pixel for pixel with the real U8g2. No device is touched.

Output: tools/tft_preview/<screen>.png (2x), tools/tft_preview/sheet.png (1:1, all screens).
"""
import concurrent.futures
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / '.pio/tft-native'
PREVIEW = ROOT / 'tools/tft_preview'


def find_u8g2():
    for env in ('native-usb', 'uart'):
        p = ROOT / '.pio/libdeps' / env / 'U8g2/src/clib'
        if (p / 'u8g2.h').exists():
            return p
    return None


def compilers():
    zig = ROOT / '.pio/native-test-tools/ziglang/zig.exe'
    cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
    if cxx:
        cc = os.environ.get('CC') or shutil.which('clang') or shutil.which('gcc') or cxx
        return [cc], [cxx]
    if zig.exists():
        target = ['-target', 'x86_64-windows-gnu']
        return [str(zig), 'cc'] + target, [str(zig), 'c++'] + target
    raise SystemExit('Native C/C++ compiler unavailable: set CC/CXX, or install Zig under .pio/native-test-tools.')


def used_font_names():
    text = (ROOT / 'src/gfx_fonts.h').read_text()
    return sorted(set(re.findall(r'extern const uint8_t (u8g2_font_\w+)\[\]', text)))


def extract_fonts(u8g2_dir, names, out):
    """u8g2_fonts.c is ~40 MB; copy just the arrays we use into a small C file."""
    want = set(names)
    chunks, found = [], set()
    start = re.compile(r'^const uint8_t (u8g2_font_\w+)\[\d*\]')
    cur = None
    with open(u8g2_dir / 'u8g2_fonts.c', 'r', encoding='latin-1') as f:
        for line in f:
            if cur is None:
                m = start.match(line)
                if m and m.group(1) in want:
                    cur = [line]
                    found.add(m.group(1))
                    if line.rstrip().endswith(';'):
                        chunks.append(''.join(cur)); cur = None
            else:
                cur.append(line)
                if line.rstrip().endswith(';'):
                    chunks.append(''.join(cur)); cur = None
    missing = want - found
    if missing:
        raise SystemExit('fonts not found in U8g2: ' + ', '.join(sorted(missing)))
    out.write_text('#include "u8g2.h"\n' + ''.join(chunks), encoding='latin-1')


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT, **kw)
    if r.returncode:
        sys.stdout.write(r.stdout + r.stderr)
        raise SystemExit('command failed: ' + ' '.join(str(c) for c in cmd[:6]) + ' ...')
    return r


def main():
    cc, cxx = compilers()
    u8g2 = find_u8g2()
    if not u8g2:
        raise SystemExit('U8g2 sources not found in .pio/libdeps (build the firmware once so PlatformIO downloads them).')
    BUILD.mkdir(parents=True, exist_ok=True)
    PREVIEW.mkdir(parents=True, exist_ok=True)

    fonts_c = BUILD / 'fonts_subset.c'
    names = used_font_names()
    stamp = ','.join(names)
    stamp_file = BUILD / 'fonts_subset.stamp'
    if not fonts_c.exists() or not stamp_file.exists() or stamp_file.read_text() != stamp:
        extract_fonts(u8g2, names, fonts_c)
        stamp_file.write_text(stamp)

    # The parts of U8g2 the font comparison needs (an SSD1306 buffer; nothing is ever sent anywhere).
    sources = [p for p in sorted(u8g2.glob('u8g2_*.c')) if p.name not in ('u8g2_fonts.c', 'u8g2_d_setup.c')]
    sources += [p for p in sorted(u8g2.glob('u8x8_*.c'))
                if not p.name.startswith('u8x8_d_') or p.name == 'u8x8_d_ssd1306_128x64_noname.c']
    sources.append(fonts_c)
    objs = []

    def compile_c(src):
        obj = BUILD / (src.stem + '.o')
        if not obj.exists() or obj.stat().st_mtime < src.stat().st_mtime:
            run(cc + ['-c', '-O1', '-w', '-fno-sanitize=undefined', '-I', str(u8g2), str(src), '-o', str(obj)])
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        objs = list(ex.map(compile_c, sources))

    exe = BUILD / ('tft-tests.exe' if os.name == 'nt' else 'tft-tests')
    run(cxx + ['-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', '-fno-sanitize=undefined', '-DWITH_U8G2',
               '-I', str(ROOT / 'src'), '-I', str(u8g2),
               str(ROOT / 'tools/tests/tft_native_test.cpp'), str(ROOT / 'src/gfx.cpp'),
               str(ROOT / 'src/tft_screens.cpp')] + [str(o) for o in objs] + ['-o', str(exe)])
    result = subprocess.run([str(exe), str(PREVIEW)], text=True, capture_output=True, cwd=ROOT)
    print(result.stdout, end='')
    if result.stderr:
        print(result.stderr, end='')
    if result.returncode:
        raise SystemExit('tft tests FAILED')
    if '--open' in sys.argv and os.name == 'nt':
        os.startfile(PREVIEW / 'sheet.png')  # noqa: S606 (our own generated file)


if __name__ == '__main__':
    main()
