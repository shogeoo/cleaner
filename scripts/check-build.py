#!/usr/bin/env python3
"""Check the actual PE's embedded recordings and runtime imports, without running it."""
import io
import pathlib
import re
import struct
import subprocess
import wave

root = pathlib.Path(__file__).resolve().parents[1]
exe = root / 'dist/DesktopPrank.exe'
b = exe.read_bytes()
u16 = lambda o: struct.unpack_from('<H', b, o)[0]
u32 = lambda o: struct.unpack_from('<I', b, o)[0]
pe = u32(0x3c)
assert b[pe:pe+4] == b'PE\0\0' and u16(pe+4) == 0x8664
opt = pe+24
assert u16(opt) == 0x20b
sections = opt+u16(pe+20)

def offset(rva):
    for i in range(u16(pe+6)):
        s = sections+40*i
        virtual_size, start, raw_size, raw = struct.unpack_from('<IIII', b, s+8)
        if start <= rva < start+max(virtual_size, raw_size):
            assert rva-start < raw_size
            return raw+rva-start
    raise AssertionError('RVA outside sections')

base = offset(u32(opt+112+16))
def children(relative):
    directory = base+relative
    count = u16(directory+12)+u16(directory+14)
    return dict(struct.unpack_from('<II', b, directory+16+8*i) for i in range(count))

recordings = children(children(0)[10] & 0x7fffffff)
expected = {1,2,3,4,5,6,7,8,9,10,11,13,15,16}
assert set(recordings) == expected
for n, directory in recordings.items():
    languages = children(directory & 0x7fffffff)
    assert len(languages) == 1
    data_entry = base+next(iter(languages.values()))
    rva, size = struct.unpack_from('<II', b, data_entry)
    blob = b[offset(rva):offset(rva)+size]
    assert blob == (root / f'assets/{n}.wav').read_bytes(), f'Resource {n} mismatch'
    with wave.open(io.BytesIO(blob)) as wav:
        assert wav.getnchannels() == 1 and wav.getsampwidth() == 2
        assert wav.getframerate() == 22050 and wav.getnframes() > 0
imports = subprocess.check_output(['x86_64-w64-mingw32-objdump', '-p', str(exe)], text=True)
for dll in re.findall(r'DLL Name: (\S+)', imports):
    assert dll.lower() in {'kernel32.dll','ole32.dll','oleaut32.dll','shell32.dll','user32.dll','winmm.dll'} or dll.lower().startswith('api-ms-win-crt-'), dll
print('PASS: Windows x64 PE; all 14 embedded recordings match; only Windows runtime imports')
