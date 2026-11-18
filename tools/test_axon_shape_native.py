"""Compile src/axon_shape.h natively and check the Axon 0x034D payload gate.

    python tools/test_axon_shape_native.py

Uses the header exactly as the firmware does. No device is touched.
"""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / '.pio/axon-native'

TEST = r'''
#include <cstdio>
#include <cstring>
#include <vector>
#include "axon_shape.h"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static std::vector<uint8_t> md(std::initializer_list<uint8_t> head, const char* tail) {
  std::vector<uint8_t> v(head); for (const char* p = tail; *p; p++) v.push_back((uint8_t)*p); return v;
}
int main() {
  const uint8_t LOW = 1, POSSIBLE = 2, LIKELY = 3;
  auto ok = md({0x4D, 0x03, 0x02}, "X85ABC123");       // company + 0x02 + 9 serial chars
  CHECK(axonMfrShape(ok.data(), ok.size()));
  auto longer = md({0x4D, 0x03, 0x02}, "X87ABC123ZZZZ");  // extra bytes after the serial are fine
  CHECK(axonMfrShape(longer.data(), longer.size()));
  auto digits = md({0x4D, 0x03, 0x02}, "012345678");
  CHECK(axonMfrShape(digits.data(), digits.size()));
  auto shortSerial = md({0x4D, 0x03, 0x02}, "X85ABC12");      // 8 chars: too short
  CHECK(!axonMfrShape(shortSerial.data(), shortSerial.size()));
  auto lower = md({0x4D, 0x03, 0x02}, "X85abc123");           // lowercase rejected
  CHECK(!axonMfrShape(lower.data(), lower.size()));
  auto punct = md({0x4D, 0x03, 0x02}, "X85-BC123");
  CHECK(!axonMfrShape(punct.data(), punct.size()));
  auto wrongType = md({0x4D, 0x03, 0x01}, "X85ABC123");
  CHECK(!axonMfrShape(wrongType.data(), wrongType.size()));
  auto otherCo = md({0x4C, 0x00, 0x02}, "X85ABC123");         // right payload, other company
  CHECK(!axonMfrShape(otherCo.data(), otherCo.size()));
  // Smartwatch-like: company ID then its own 6 address bytes, then zeros.
  const uint8_t watch[] = {0x4D, 0x03, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0, 0, 0, 0, 0, 0};
  CHECK(!axonMfrShape(watch, sizeof(watch)));
  CHECK(!axonMfrShape(ok.data(), 11));                        // truncated by one byte
  CHECK(axonMfrShape(ok.data(), 12));                         // exactly the minimum
  CHECK(!axonMfrShape(nullptr, 12));
  CHECK(!axonMfrShape(ok.data(), 0));
  // Tier policy
  CHECK(axonCompanyTier(0x034D, LIKELY, true, LOW) == LIKELY);     // shaped: unchanged
  CHECK(axonCompanyTier(0x034D, LIKELY, false, LOW) == LOW);       // unshaped: demoted
  CHECK(axonCompanyTier(0x034D, POSSIBLE, false, LOW) == LOW);
  CHECK(axonCompanyTier(0x034D, LOW, false, LOW) == LOW);          // never raised
  CHECK(axonCompanyTier(0x034D, 0, false, LOW) == 0);
  CHECK(axonCompanyTier(0x04EC, POSSIBLE, false, LOW) == POSSIBLE); // other companies untouched
  CHECK(axonCompanyTier(0x09C8, LIKELY, false, LOW) == LIKELY);
  if (fails) { std::printf("%d failed\n", fails); return 1; }
  std::printf("axon shape: all checks passed\n");
  return 0;
}
'''


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / 'test.cpp').write_text(TEST, encoding='utf-8')
    zig = ROOT / '.pio/native-test-tools/ziglang/zig.exe'
    cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
    if cxx:
        compiler = [cxx]
    elif zig.exists():
        compiler = [str(zig), 'c++', '-target', 'x86_64-windows-gnu']
    else:
        raise SystemExit('Native C++ compiler unavailable: set CXX to clang++/g++, or install Zig locally.')
    exe = BUILD / ('axon-test.exe' if os.name == 'nt' else 'axon-test')
    subprocess.run(compiler + ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                               str(BUILD / 'test.cpp'), '-o', str(exe)], check=True, cwd=ROOT)
    result = subprocess.run([str(exe)], text=True, capture_output=True, cwd=ROOT)
    print(result.stdout, end='')
    if result.returncode:
        raise SystemExit('axon shape tests FAILED')


if __name__ == '__main__':
    main()
