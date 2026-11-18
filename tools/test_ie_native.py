"""Check that firmware ieSigMatches() and tools/watchlist.py ie_sig_matches() agree.

    python tools/test_ie_native.py

The C++ function is cut out of src/detector.cpp as written (nothing is copied by hand), compiled
natively, and fed (a) every probe-IE vector in tools/tests/alert_cases.json and (b) a few thousand
generated IE lists built from plain tags, vendor elements and the Flock anchor/tail. Both
implementations must give the same answer on every one. No device is touched.
"""
import importlib.util
import itertools
import json
import os
from pathlib import Path
import random
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / '.pio/ie-native'

HARNESS = r'''
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <string>
%s
int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t a = line.find('\t'), b = line.find('\t', a + 1);
    if (a == std::string::npos || b == std::string::npos) return 2;
    std::string sig = line.substr(0, a), rule = line.substr(a + 1, b - a - 1), ssid = line.substr(b + 1);
    std::putchar(ieSigMatches(sig.c_str(), rule.c_str(), ssid.c_str()) ? '1' : '0');
    std::putchar('\n');
  }
  return 0;
}
'''


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def extract_cpp():
    text = (ROOT / 'src/detector.cpp').read_text(encoding='utf-8')
    start = text.index('static const char kFlockIeAnchor[]')
    end = text.index('// DATA rows:', start)
    body = text[start:end]
    assert 'static bool ieSigMatches(' in body and body.rstrip().endswith('}')
    return body


def build_cases(wl, rule):
    vectors = []
    for c in json.loads((ROOT / 'tools/tests/alert_cases.json').read_text(encoding='utf-8')):
        if c['frame_type'] == 'PROBE_REQ' and c['extra'].startswith('ie='):
            vectors.append((c['extra'][3:], rule, c['ssid']))
    plain = ['1', '2', '3', '12', '45', '50', '127', '191']
    odd = ['221:0050f204', '221:506f9a09', '221:0050f208000000', '255', '221:12345678', '221:506f9a1603010300']
    anchor = wl.FLOCK_IE_ANCHOR
    alphabet = plain + odd
    tail = rule[rule.index(anchor):].split(',')
    tails = [tail, tail[:-1], tail[:1], [tail[0], tail[2], tail[1]] + tail[3:], tail + ['9'], [anchor], [anchor + '00'] + tail[1:]]
    prefixes = [()]
    for n in range(1, 4):
        prefixes += list(itertools.product(alphabet, repeat=n))
    rng = random.Random(20261115)
    for n in range(4, 8):
        prefixes += [tuple(rng.choice(alphabet) for _ in range(n)) for _ in range(400)]
    generated = [(','.join(list(pre) + t), rule, '') for pre in prefixes for t in tails]
    rng.shuffle(generated)
    generated = generated[:20000]
    named = [(sig, r, 'Home') for sig, r, _ in generated[:500]]
    edge = [('', rule, ''), (rule, rule, 'Home'), ('2,12,127', rule, '')]
    return vectors + generated + named + edge


def main():
    wl = load('watchlist_scan', 'tools/watchlist.py')
    _, sigs = wl.load_signatures()
    rule = next(s['value'] for s in sigs if s['match_type'] == 'probe_ie_signature')
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / 'harness.cpp').write_text(HARNESS % extract_cpp(), encoding='utf-8')

    zig = ROOT / '.pio/native-test-tools/ziglang/zig.exe'
    cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
    if cxx:
        compiler = [cxx]
    elif zig.exists():
        compiler = [str(zig), 'c++', '-target', 'x86_64-windows-gnu']
    else:
        raise SystemExit('Native C++ compiler unavailable: set CXX to clang++/g++, or install Zig locally.')
    exe = BUILD / ('ie-test.exe' if os.name == 'nt' else 'ie-test')
    subprocess.run(compiler + ['-std=c++17', '-Wall', '-Wextra', '-Werror', str(BUILD / 'harness.cpp'), '-o', str(exe)],
                   check=True, cwd=ROOT)

    cases = build_cases(wl, rule)
    result = subprocess.run([str(exe)], input='\n'.join('\t'.join(c) for c in cases) + '\n',
                            text=True, capture_output=True, cwd=ROOT)
    if result.returncode:
        raise SystemExit(f'harness failed (exit {result.returncode}): {result.stderr}')
    got = result.stdout.split()
    assert len(got) == len(cases), (len(got), len(cases))
    mismatches = []
    matched = 0
    for (sig, r, ssid), g in zip(cases, got):
        expected = wl.ie_sig_matches(sig, r, ssid)
        matched += expected
        if (g == '1') != expected:
            mismatches.append((sig, ssid, g, expected))
    for m in mismatches[:10]:
        print('MISMATCH sig=%r ssid=%r c++=%s python=%s' % m)
    if mismatches:
        raise SystemExit(f'{len(mismatches)} of {len(cases)} cases differ between detector.cpp and watchlist.py')
    # Sanity: the generated set must contain both outcomes, or it proves nothing.
    assert 0 < matched < len(cases), matched
    flock = rule
    assert wl.ie_sig_matches(flock, rule, '')
    print(f'ie matcher: C++ == Python on {len(cases)} cases ({matched} match, {len(cases) - matched} do not)')
    print('ie native tests passed')


if __name__ == '__main__':
    main()
