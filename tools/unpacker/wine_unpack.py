#!/usr/bin/env python3
"""
Unpack a .UBX-protected PE by letting it self-unpack under a real 32-bit Wine,
then dumping the unpacked image from the live process via /proc/<pid>/mem.

This is the approach that actually completes the unpack: the protector's final
phase executes real imported-DLL code (user32, ...), which only works inside a
fully linked Windows API environment. Wine provides exactly that, so the stub
decompresses the image, rebuilds the import table and jumps to the OEP on its
own; we just snapshot memory while it runs.

Prereqs:
    apt-get install -y --no-install-recommends wine32:i386   # + libgd3:i386
    pip install pefile
Setup:
    - WINEPREFIX with WINEARCH=win32 (wineboot -u)
    - the exe + game DLLs (steam_api, libcef, bink2w32, msvcr*, ...) in one dir
    - optionally Goldberg steam_api.dll so the Steam check passes

Usage:
    WINEPREFIX=/tmp/wp WINEARCH=win32 python3 wine_unpack.py /path/to/run/Game.exe
"""
import subprocess, os, sys, time, struct

IMAGE_BASE = 0x400000
IMAGE_SIZE = 0x19c7000          # >= SizeOfImage of the target
TEXT_RVA   = 0x1000             # sample window to detect "is .text decompressed yet"

def find_pids(name):
    out = subprocess.run(['pgrep', '-f', name], capture_output=True, text=True).stdout.split()
    return [int(x) for x in out]

def main(exe):
    rundir = os.path.dirname(os.path.abspath(exe))
    name   = os.path.basename(exe)
    env    = dict(os.environ)
    env.setdefault('WINEDEBUG', '-all')
    env.setdefault('WINEDLLOVERRIDES', 'mscoree,mshtml=d')
    p = subprocess.Popen(['wine', name], cwd=rundir,
                         stdout=open('/tmp/wine_unpack_out.log', 'wb'),
                         stderr=subprocess.STDOUT, env=env)
    best = None
    time.sleep(1.5)
    for _ in range(200):                       # ~20s window
        for pid in find_pids(name):
            try:
                if '00400000' not in open(f'/proc/{pid}/maps').read():
                    continue
                with open(f'/proc/{pid}/mem', 'rb', 0) as mem:
                    mem.seek(IMAGE_BASE)
                    data = mem.read(IMAGE_SIZE)
            except Exception:
                continue
            nz = sum(1 for b in data[TEXT_RVA:TEXT_RVA + 0x4000] if b)
            if best is None or nz > best[1]:
                best = (bytes(data), nz, pid)
        if p.poll() is not None and not find_pids(name):
            break
        time.sleep(0.1)
    try: p.wait(timeout=5)
    except Exception: p.kill()
    if not best:
        print("no unpacked image captured"); return 1
    open('/tmp/wine_dump.bin', 'wb').write(best[0])
    print(f"captured unpacked image: text_nz={best[1]} pid={best[2]} -> /tmp/wine_dump.bin")
    rebuild_pe('/tmp/wine_dump.bin', '/tmp/wine_unpacked.exe')
    return 0

def rebuild_pe(virt_path, out_path):
    """Turn the virtual memory image into a file-aligned PE for analysis tools."""
    import pefile
    raw = open(virt_path, 'rb').read()
    pe = pefile.PE(data=raw)
    fa = 0x200
    cur = pe.OPTIONAL_HEADER.SizeOfHeaders
    blobs = []
    for s in pe.sections:
        vs = s.Misc_VirtualSize
        data = raw[s.VirtualAddress:s.VirtualAddress + vs]
        rawsz = (len(data) + fa - 1) & ~(fa - 1)
        data = data.ljust(rawsz, b'\x00')
        s.PointerToRawData = cur; s.SizeOfRawData = rawsz
        blobs.append(data); cur += rawsz
    hdr = pe.write()[:pe.OPTIONAL_HEADER.SizeOfHeaders]
    with open(out_path, 'wb') as f:
        f.write(hdr)
        for b in blobs: f.write(b)
    print(f"rebuilt PE -> {out_path}  (NOTE: header EP still points at the stub; "
          f"the real OEP + a portable import table are the remaining Scylla-style step)")

if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else '/tmp/run/MightyQuest.exe'))
