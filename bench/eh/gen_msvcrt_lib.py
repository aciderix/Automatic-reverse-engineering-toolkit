#!/usr/bin/env python3
"""Parse the export table of Wine's msvcrt.dll and emit a .def (stdout), so
llvm-dlltool can build an i386 import library. This is what lets clang MSVC-ABI
C++ objects (which reference _CxxThrowException / __CxxFrameHandler3 / type_info)
link into a PE whose imports resolve to msvcrt at runtime — under both Wine (oracle)
and ARET. Usage: gen_msvcrt_lib.py /path/to/msvcrt.dll > msvcrt.def"""
import sys, struct
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
assert d[pe:pe+4] == b'PE\0\0'
opt = pe + 24
nsec = struct.unpack_from('<H', d, pe+6)[0]
sh = opt + struct.unpack_from('<H', d, pe+20)[0]
secs = []
for i in range(nsec):
    o = sh + i*40
    vsz, va, rsz, rptr = struct.unpack_from('<IIII', d, o+8)
    secs.append((va, vsz, rptr, rsz))
def r2o(rva):
    for va, vsz, rptr, rsz in secs:
        if va <= rva < va + max(vsz, rsz):
            return rptr + (rva - va)
    return None
erva = struct.unpack_from('<I', d, opt+96)[0]           # DataDirectory[0] = export
eo = r2o(erva)
nameCount = struct.unpack_from('<I', d, eo+24)[0]
enpt = r2o(struct.unpack_from('<I', d, eo+32)[0])       # AddressOfNames
print("LIBRARY msvcrt.dll"); print("EXPORTS")
for i in range(nameCount):
    no = r2o(struct.unpack_from('<I', d, enpt+i*4)[0])
    print(d[no:d.index(b'\0', no)].decode('latin1'))
