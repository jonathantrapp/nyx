"""Compile/run portable firmware regressions. Needs CXX, clang++, g++, or local Zig.

    python tools/test_storage_native.py

No device, captures, filesystem image or NVS data is accessed.
"""
import csv
import io
import os
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parent.parent


def main():
    zig=ROOT/'.pio/native-test-tools/ziglang/zig.exe'
    cxx=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
    if cxx:
        compiler=[cxx]
    elif zig.exists():
        compiler=[str(zig),'c++','-target','x86_64-windows-gnu']
    else:
        raise SystemExit('Native C++ compiler unavailable: set CXX to clang++/g++, or install Zig locally.')
    build=ROOT/'.pio/storage-native'; build.mkdir(parents=True,exist_ok=True)
    exe=build/('storage-tests.exe' if os.name=='nt' else 'storage-tests')
    command=compiler+['-std=c++17','-Wall','-Wextra','-Werror',
        '-I'+str(ROOT/'tools/tests/native_stubs'),'-I'+str(ROOT/'src'),'-I'+str(ROOT/'include'),
        str(ROOT/'tools/tests/storage_native_test.cpp'),str(ROOT/'src/summary_writer.cpp'),'-o',str(exe)]
    subprocess.run(command,check=True,cwd=ROOT)
    result=subprocess.run([str(exe)],check=True,text=True,capture_output=True,cwd=ROOT)
    print(result.stdout.splitlines()[0])
    rows=list(csv.reader(io.StringIO(result.stdout.split('\n',1)[1])))
    assert len(rows)==1 and len(rows[0])==10 and rows[0][2]=='SUMMARY', rows
    assert rows[0][1]=='' and all(rows[0][i]=='' for i in (3,4,5,7,8))
    values=dict(pair.split('=',1) for pair in rows[0][9].split())
    assert values['wifi_heard']=='4294967295' and values['det_fleet_modem']=='4294967295'
    assert values['unique_capped']=='1' and values['partial']=='1'
    print('SUMMARY CSV round-trip: 10 columns, empty device fields, full-width counts OK')


if __name__=='__main__':
    main()
