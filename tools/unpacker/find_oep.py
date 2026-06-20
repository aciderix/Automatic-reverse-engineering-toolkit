#!/usr/bin/env python3
"""
Recover the original entry point (OEP) of the unpacked MSVC binary without
single-stepping, by anchoring on the CRT startup signature.

Method (works once you have the decompressed image + a few resolved IAT slots,
e.g. from harvest_imports_emulated.py):
  1. __security_init_cookie seeds entropy from a fixed set of APIs
     (QueryPerformanceCounter, GetTickCount, GetCurrentThreadId/ProcessId,
     GetSystemTimeAsFileTime). Find the function that calls those IAT slots.
  2. Its single caller is __tmainCRTStartup (chkstk + SEH + /GS cookie +
     GetStartupInfo).
  3. The wrapper that calls __tmainCRTStartup and has *no* internal callers is
     the PE entry point = OEP (e.g. _WinMainCRTStartup).

For the case study target the OEP came out at RVA 0x6545e0.
"""
import struct, re, sys
import capstone

def callers_of(img, rva, lo=0x1000, hi=0xa00000):
    out=[]
    for m in re.finditer(rb'\xe8', img[lo:hi]):
        o=m.start()+lo
        rel=struct.unpack_from('<i',img,o+1)[0]
        if (o+5+rel)&0xffffffff==rva: out.append(o)
    return out

def func_start(img, callsite, back=0x300):
    region=img[max(0,callsite-back):callsite]; last=None
    for m in re.finditer(rb'\xcc{3,}', region):
        last=max(0,callsite-back)+m.end()
    return last

def find_oep(image_path, cookie_func_rva):
    img=open(image_path,'rb').read()
    cs=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
    tmain_calls=callers_of(img, cookie_func_rva)
    tmain=func_start(img, tmain_calls[0]) if tmain_calls else None
    if not tmain: return None
    for c in callers_of(img, tmain):
        wrapper=func_start(img, c)
        if wrapper and len(callers_of(img, wrapper))==0:
            return wrapper
    return None

if __name__=='__main__':
    # pass: <image> <rva-of-a-function-that-calls-QueryPerformanceCounter+GetTickCount>
    img=sys.argv[1] if len(sys.argv)>1 else '/tmp/st_image.bin'
    cookie=int(sys.argv[2],16) if len(sys.argv)>2 else 0x6676d0
    oep=find_oep(img, cookie)
    print("OEP RVA:", hex(oep) if oep else "not found")
