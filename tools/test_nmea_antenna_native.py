"""Compile src/nmea.h natively and check the module antenna-status ($xxTXT) parsing.

    python tools/test_nmea_antenna_native.py

Uses the header exactly as the firmware does. No device is touched.
"""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / '.pio/nmea-antenna-native'

TEST = r'''
#include <cstdio>
#include <cstring>
#include <string>
#include "nmea.h"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::string sentence(const std::string& body) {
  uint8_t sum = 0; for (char c : body) sum ^= (uint8_t)c;
  char tail[8]; std::snprintf(tail, sizeof(tail), "*%02X\r\n", sum);
  return "$" + body + tail;
}
static bool feedAll(NmeaParser& p, const std::string& s) {
  bool any = false; for (char c : s) any |= p.feed(c); return any;
}
int main() {
  NmeaParser p;
  CHECK(p.fix().antenna == 0);                                          // never reported
  CHECK(std::strcmp(nmeaAntennaName(p.fix().antenna), "unknown") == 0);

  // The exact line the owner's ATGM336H prints (checksum *35 as seen on the unit).
  CHECK(!feedAll(p, "$GPTXT,01,01,01,ANTENNA OK*35\r\n"));              // never counts as a fix update
  CHECK(p.fix().antenna == 1);
  CHECK(std::strcmp(nmeaAntennaName(1), "ok") == 0);
  CHECK(p.fix().sentences == 1);                                         // it is a valid sentence

  feedAll(p, sentence("GPTXT,01,01,01,ANTENNA OPEN"));
  CHECK(p.fix().antenna == 2); CHECK(std::strcmp(nmeaAntennaName(2), "open") == 0);
  feedAll(p, sentence("GPTXT,01,01,01,ANTENNA SHORT"));
  CHECK(p.fix().antenna == 3); CHECK(std::strcmp(nmeaAntennaName(3), "short") == 0);
  feedAll(p, sentence("GPTXT,01,01,01,ANTENNA OK"));
  CHECK(p.fix().antenna == 1);                                           // plugged back in

  // Other talkers carry the same text.
  feedAll(p, sentence("GNTXT,01,01,01,ANTENNA OPEN")); CHECK(p.fix().antenna == 2);
  feedAll(p, sentence("BDTXT,01,01,01,ANTENNA OK")); CHECK(p.fix().antenna == 1);

  // Things that must NOT change the state.
  const uint32_t badBefore = p.fix().badChecksums;
  feedAll(p, "$GPTXT,01,01,01,ANTENNA OPEN*00\r\n");                     // wrong checksum
  CHECK(p.fix().antenna == 1); CHECK(p.fix().badChecksums == badBefore + 1);
  feedAll(p, sentence("GPTXT,01,01,01,HELLO WORLD")); CHECK(p.fix().antenna == 1);     // other text
  feedAll(p, sentence("GPTXT,01,01,01,ANTENNA WEIRD")); CHECK(p.fix().antenna == 1);   // unknown state
  feedAll(p, sentence("GPTXT,01,01,01,ANTENNA")); CHECK(p.fix().antenna == 1);         // no state word
  feedAll(p, sentence("GPTXT,01,01,01,ANTENNA OKAY")); CHECK(p.fix().antenna == 1);    // not a prefix match
  feedAll(p, sentence("GPTXT,01,01,01,antenna open")); CHECK(p.fix().antenna == 1);    // case sensitive
  feedAll(p, sentence("GPTXT,01,01,01,ANTEXXX OPEN")); CHECK(p.fix().antenna == 1);    // whole prefix must match
  feedAll(p, sentence("GPTXT,01")); CHECK(p.fix().antenna == 1);                       // too few fields
  feedAll(p, sentence("GPTXT")); CHECK(p.fix().antenna == 1);

  // Position handling is untouched: RMC and GGA still work around TXT lines.
  CHECK(feedAll(p, sentence("GNRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,230394,,,A")));
  CHECK(p.fix().valid && p.fix().rmcCount == 1);
  feedAll(p, sentence("GPTXT,01,01,01,ANTENNA OPEN"));
  CHECK(p.fix().antenna == 2 && p.fix().valid);                          // antenna does not clear the fix
  CHECK(feedAll(p, sentence("GNGGA,123519.00,4807.038,N,01131.000,E,1,12,0.9,545.4,M,46.9,M,,")));
  CHECK(p.fix().sats == 12);

  if (fails) { std::printf("%d failed\n", fails); return 1; }
  std::printf("nmea antenna: all checks passed\n");
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
    exe = BUILD / ('nmea-antenna-test.exe' if os.name == 'nt' else 'nmea-antenna-test')
    subprocess.run(compiler + ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                               str(BUILD / 'test.cpp'), '-o', str(exe)], check=True, cwd=ROOT)
    result = subprocess.run([str(exe)], text=True, capture_output=True, cwd=ROOT)
    print(result.stdout, end='')
    if result.returncode:
        raise SystemExit('nmea antenna tests FAILED')


if __name__ == '__main__':
    main()
