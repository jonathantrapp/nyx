"""Test the GPS parser, the camera map builder and the camera alert logic on the PC.

    python tools/test_camera_native.py

Builds a tiny fixture map with tools/build_camera_map.py, compiles tools/tests/camera_native_test.cpp
(which includes src/nmea.h and src/camera_map.h unchanged), and runs it. If cameras/cameras.bin
exists it is also loaded and spot-checked. No device is touched.
"""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def load_builder():
    spec = importlib.util.spec_from_file_location('build_camera_map', ROOT / 'tools/build_camera_map.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def node(i, lat, lon, **tags):
    return {'type': 'node', 'id': i, 'lat': lat, 'lon': lon,
            'tags': dict({'man_made': 'surveillance', 'surveillance:type': 'ALPR'}, **tags)}


def test_builder(b):
    p = b.parse_one_direction
    assert p('270') == 270 and p('NE') == 45 and p('nne') in (22, 23)
    assert p('140-160') == 150 and p('350-10') == 0 and p('360') == 0 and p('-90') == 270
    assert p('') is None and p('forward') is None
    assert b.parse_direction({'direction': '90;270'}) == (90, b.FLAG_DIR | b.FLAG_MULTI)
    assert b.parse_direction({}) == (b.DIR_UNKNOWN, 0)
    assert b.classify_maker({'manufacturer': 'Flock Safety'}) == b.MAKER_FLOCK
    assert b.classify_maker({'operator': 'Flock Safety'}) == b.MAKER_FLOCK
    assert b.classify_maker({'manufacturer': 'Motorola Solutions'}) == b.MAKER_MOTOROLA
    assert b.classify_maker({'manufacturer': 'Genetec'}) == b.MAKER_OTHER
    assert b.classify_maker({}) == b.MAKER_UNKNOWN
    print('builder: direction and maker parsing OK')


def build_fixture(b, out):
    # A road runs north along lon -75.0 from lat 40.000. See tools/tests/camera_native_test.cpp.
    elements = [
        node(1, 40.0050, -75.0000, direction='180', manufacturer='Flock Safety'),      # A on the road
        node(2, 40.0052, -75.00005, direction='0', manufacturer='Flock Safety'),       # B, 22 m past A
        node(3, 40.0030, -74.9965, direction='270', manufacturer='Flock Safety'),      # C, 300 m east
        node(4, 39.9995, -75.0000, direction='0', manufacturer='Flock Safety'),        # D, behind the start
        node(5, 40.0090, -75.0000, direction='90;270'),                                # E, further up, unknown maker
        node(6, 41.0000, -75.0000, direction='NE', manufacturer='Genetec'),
        node(7, 41.0010, -75.0000, direction='140-160', manufacturer='Axon Enterprise'),
    ]
    doc = {'osm3s': {'timestamp_osm_base': '2026-11-10T17:36:36Z'}, 'elements': elements}
    j = out / 'fixture.json'
    j.write_text(json.dumps(doc))
    b.main([str(j), '-o', str(out / 'fixture.bin')])
    return out / 'fixture.bin'


def main():
    b = load_builder()
    test_builder(b)
    build = ROOT / '.pio/camera-native'
    build.mkdir(parents=True, exist_ok=True)
    fixture = build_fixture(b, build)

    zig = ROOT / '.pio/native-test-tools/ziglang/zig.exe'
    cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
    if cxx:
        compiler = [cxx]
    elif zig.exists():
        compiler = [str(zig), 'c++', '-target', 'x86_64-windows-gnu']
    else:
        raise SystemExit('Native C++ compiler unavailable: set CXX to clang++/g++, or install Zig locally.')
    exe = build / ('camera-tests.exe' if os.name == 'nt' else 'camera-tests')
    subprocess.run(compiler + ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'),
                               str(ROOT / 'tools/tests/camera_native_test.cpp'), '-o', str(exe)],
                   check=True, cwd=ROOT)
    args = [str(exe), str(fixture)]
    real = ROOT / 'cameras/cameras.bin'
    if real.exists():
        args.append(str(real))
    result = subprocess.run(args, text=True, capture_output=True, cwd=ROOT)
    print(result.stdout, end='')
    if result.returncode:
        raise SystemExit('camera tests FAILED')


if __name__ == '__main__':
    main()
