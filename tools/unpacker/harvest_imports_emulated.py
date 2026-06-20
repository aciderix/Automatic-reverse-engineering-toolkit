import builtins, threading, sys, os, time, struct, collections, json
_ans=iter(['yes','ep','']); builtins.input=lambda p='': next(_ans,'')
import unicorn
from unicorn import *
from unicorn.x86_const import *
import unipacker.apicalls as A
import unipacker.core as C
from unipacker.utils import get_string
import pefile, capstone
cs=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)

IB=0x400000; TEXT_LO=0x401000; TEXT_HI=0xdeb14f
UNIP={'kernel32':0x755D0000,'kernelbase':0x73D00000,'ntdll':0x77400000}
DLLDIR='/tmp/dllset'
DLLMAP={}    # normname -> (base, size, path)
def norm(n):
    n=(n or '').lower().replace('\x00',''); n=n.rsplit('\\',1)[-1].rsplit('/',1)[-1]
    return n[:-4] if n.endswith('.dll') else n

# Pre-compute DLL memory images and assigned bases (mapped into uc later)
def prepare_dlls():
    base=0x10000000
    out={}
    for fn in sorted(os.listdir(DLLDIR)):
        if not fn.lower().endswith('.dll'): continue
        nm=norm(fn); path=os.path.join(DLLDIR,fn)
        try:
            pe=pefile.PE(path, fast_load=True)
            img=pe.get_memory_mapped_image(ImageBase=base)
            size=(len(img)+0xffff)&~0xffff
            out[nm]=(base, size, bytes(img))
            base+=size+0x10000
        except Exception as e:
            sys.stdout.write(f"  dll prep fail {fn}: {e}\n")
    return out

FAKE_BASE=[0x68000000]
def build_fake(uc, name):
    base=FAKE_BASE[0]; FAKE_BASE[0]+=0x100000
    img=bytearray(0x1000); img[0:2]=b'MZ'; struct.pack_into('<I',img,0x3c,0x80)
    o=0x80; img[o:o+4]=b'PE\x00\x00'
    struct.pack_into('<HHIIIHH',img,o+4,0x14c,0,0,0,0,0xe0,0x2102)
    oh=o+24; struct.pack_into('<H',img,oh,0x10b); struct.pack_into('<I',img,oh+28,base)
    struct.pack_into('<I',img,oh+56,0x10000); struct.pack_into('<I',img,oh+92,16)
    struct.pack_into('<II',img,oh+96,0x300,0x40)
    struct.pack_into('<I',img,0x300+12,0x3a0); struct.pack_into('<I',img,0x300+16,1)
    nm=(norm(name)+'.dll').encode()+b'\x00'; img[0x3a0:0x3a0+len(nm)]=nm
    uc.mem_map(base,0x10000); uc.mem_write(base,bytes(img)); return base

def mod_wrapper():
    def wrapper(self, uc, esp, log):
        ret_addr, ptr = struct.unpack("<II", uc.mem_read(esp,8)); oe=esp+8
        if not ptr: base=self.base_addr
        else:
            name=norm(get_string(ptr,uc))
            if name in UNIP: base=UNIP[name]
            elif name in DLLMAP: base=DLLMAP[name][0]
            elif name in self._fakes: base=self._fakes[name]
            else: base=build_fake(uc,name); self._fakes[name]=base
            self.module_handles[base]=name
        uc.mem_write(oe-4, struct.pack("<I",ret_addr)); return base, oe-4
    return wrapper
for n in ['GetModuleHandleA','GetModuleHandleW','LoadLibraryA','LoadLibraryW','LoadLibraryExA','LoadLibraryExW']:
    A.apicall_mapping[n]=mod_wrapper()
def w_localalloc():
    def wrapper(self, uc, esp, log):
        ret_addr, flags, size = struct.unpack("<III", uc.mem_read(esp,12)); oe=esp+12
        addr=self.alloc(False, max(size,1), uc)
        uc.mem_write(oe-4, struct.pack("<I",ret_addr)); return addr, oe-4
    return wrapper
def w_free():
    def wrapper(self, uc, esp, log):
        ret_addr, h = struct.unpack("<II", uc.mem_read(esp,8)); oe=esp+8
        uc.mem_write(oe-4, struct.pack("<I",ret_addr)); return 0, oe-4
    return wrapper
A.apicall_mapping['LocalAlloc']=w_localalloc(); A.apicall_mapping['LocalReAlloc']=w_localalloc()
A.apicall_mapping['LocalFree']=w_free(); A.apicall_mapping['GlobalFree']=w_free()
_oi=A.WinApiCalls.__init__
def _init(self,engine):
    _oi(self,engine); self._fakes={}
    for n,b in UNIP.items(): self.module_handles[b]=n
A.WinApiCalls.__init__=_init

cnt=[0]; faults=[0]; oep=[None]
ring=collections.deque(maxlen=48); prog=open('/tmp/prog9.txt','w')
def custom_dump(uc, oep_rva, path):
    pe=pefile.PE('/tmp/mq.exe'); pe.OPTIONAL_HEADER.AddressOfEntryPoint=oep_rva
    fa=0x200; cur=pe.OPTIONAL_HEADER.SizeOfHeaders; blobs=[]
    for s in pe.sections:
        vs=s.Misc_VirtualSize
        try: data=bytes(uc.mem_read(IB+s.VirtualAddress, vs))
        except Exception: data=b'\x00'*vs
        raw=(len(data)+fa-1)&~(fa-1); data=data.ljust(raw,b'\x00')
        s.PointerToRawData=cur; s.SizeOfRawData=raw; blobs.append(data); cur+=raw
    hdr=pe.write()[:pe.OPTIONAL_HEADER.SizeOfHeaders]
    with open(path,'wb') as f:
        f.write(hdr)
        for b in blobs: f.write(b)
    return os.path.getsize(path)

_derailed=[False]
def lean_code(self, uc, address, size, user_data):
    cnt[0]+=1
    if address < 0x100000 and not _derailed[0]:
        _derailed[0]=True
        src=ring[-1] if ring else 0
        sys.stdout.write(f"\n### DERAIL: EIP entered stack {hex(address)} from {hex(src)} at cnt={cnt[0]} ###\n")
        RG=[('eax',UC_X86_REG_EAX),('ebx',UC_X86_REG_EBX),('ecx',UC_X86_REG_ECX),('edx',UC_X86_REG_EDX),('esi',UC_X86_REG_ESI),('edi',UC_X86_REG_EDI),('ebp',UC_X86_REG_EBP),('esp',UC_X86_REG_ESP)]
        sys.stdout.write("  regs: "+" ".join(f"{n}={hex(uc.reg_read(c))}" for n,c in RG)+"\n")
        sys.stdout.write("  source-side ring (last 16 stub EIPs):\n")
        for a in list(ring)[-16:]:
            try: ins=next(cs.disasm(bytes(uc.mem_read(a,15)),a)); sys.stdout.write(f"     {hex(a)}: {ins.mnemonic} {ins.op_str}\n")
            except Exception: sys.stdout.write(f"     {hex(a)}: <unmapped>\n")
        # dump stack around esp to see the bad return address pushed
        esp=uc.reg_read(UC_X86_REG_ESP)
        sys.stdout.write(f"  stack @esp {hex(esp)}:\n")
        import struct as _st
        for off in range(-8,32,4):
            try: v=_st.unpack('<I',uc.mem_read(esp+off,4))[0]; sys.stdout.write(f"     [esp{off:+d}]={hex(v)}\n")
            except Exception: pass
        import struct as _s2
        def rd(a,n):
            try: return bytes(uc.mem_read(a,n))
            except Exception: return b""
        def rdd(a):
            b=rd(a,4); return _s2.unpack("<I",b)[0] if len(b)==4 else None
        sys.stdout.write(f"  [0x144b7c14] (fnptr slot) = {hex(rdd(0x144b7c14)) if rdd(0x144b7c14) is not None else '?'}\n")
        sys.stdout.write(f"  [0x144b0070] (arg0)       = {hex(rdd(0x144b0070)) if rdd(0x144b0070) is not None else '?'}\n")
        sstr=rd(0x1449243e,64); sys.stdout.write(f"  str@0x1449243e = {sstr.split(bytes([0]))[0]!r}\n")
        # what is at the bad target 0xb8e10 and around the fnptr table
        sys.stdout.write(f"  bytes@fnptr table 0x144b7c00: {rd(0x144b7c00,0x40).hex()}\n")
        # is 0xb8e10 inside any mapped DLL? print nearby
        tgt=uc.reg_read(UC_X86_REG_ESI)
        sys.stdout.write(f"  call target esi={hex(tgt)}; bytes there: {rd(tgt,16).hex()}\n")
        sys.stdout.flush()  # continue to capture writes
    ring.append(address)
    if cnt[0]%100000000==0:
        prog.write(f"{cnt[0]//1000000}M eip={hex(address)} fakes={list(self.apicall_handler._fakes)} faults={faults[0]}\n"); prog.flush()
    h=self.apicall_handler.hooks
    if address in h:
        esp=uc.reg_read(UC_X86_REG_ESP); name=h[address]
        ret,esp=self.apicall_handler.apicall(address,name,uc,esp,False)
        if ret is not None: uc.mem_write(self.HOOK_ADDR, struct.pack("<I",ret&0xffffffff))
        uc.reg_write(UC_X86_REG_ESP,esp); return
    if TEXT_LO<=address<TEXT_HI:
        oep[0]=address
        sys.stdout.write(f"*** OEP REACHED {hex(address)} at {cnt[0]} ***\n"); sys.stdout.flush()
        prog.write(f"*** OEP {hex(address)} at {cnt[0]} ***\n"); prog.flush()
        try:
            sz=custom_dump(uc, address-IB, '/tmp/unpacked_clean.exe')
            sys.stdout.write(f"clean dump {sz} -> /tmp/unpacked_clean.exe\n")
            # save IAT-relevant memory: dump .text..rsrc already in file; also dump DLL base map
            json.dump({k:[v[0],v[1]] for k,v in DLLMAP.items()}, open('/tmp/dllmap.json','w'))
            # dump the whole image region 0x400000.. for IAT analysis
            img=bytes(uc.mem_read(IB, 0x19c7000))
            open('/tmp/oep_image.bin','wb').write(img)
            sys.stdout.write("saved dllmap.json + oep_image.bin\n"); sys.stdout.flush()
        except Exception as e: sys.stdout.write(f"dump err {e}\n"); sys.stdout.flush()
        uc.emu_stop(); self.emulator_event.clear()
        for c in self.clients: c.emu_done()
C.UnpackerEngine.hook_code=lean_code
C.UnpackerEngine.hook_mem_access=lambda self,*a: None
def patched_invalid(self,uc,access,address,size,value,user_data):
    faults[0]+=1
    eip=uc.reg_read(UC_X86_REG_EIP)
    sys.stdout.write(f"\n=== MEMINVALID#{faults[0]} acc={access} addr={hex(address)} sz={size} eip={hex(eip)} cnt={cnt[0]} ===\n")
    RG=[('eax',UC_X86_REG_EAX),('ebx',UC_X86_REG_EBX),('ecx',UC_X86_REG_ECX),('edx',UC_X86_REG_EDX),('esi',UC_X86_REG_ESI),('edi',UC_X86_REG_EDI),('ebp',UC_X86_REG_EBP),('esp',UC_X86_REG_ESP)]
    sys.stdout.write("  regs: "+" ".join(f"{n}={hex(uc.reg_read(c))}" for n,c in RG)+"\n")
    sys.stdout.write("  full ring (48):\n")
    for a in list(ring):
        try: ins=next(cs.disasm(bytes(uc.mem_read(a,15)),a)); sys.stdout.write(f"     {hex(a)}: {ins.mnemonic} {ins.op_str}\n")
        except Exception: sys.stdout.write(f"     {hex(a)}: <unmapped>\n")
    sys.stdout.flush()
    uc.emu_stop(); return False
C.UnpackerEngine.hook_mem_invalid=patched_invalid

print("preparing DLLs...")
PREP=prepare_dlls()
print(f"prepared {len(PREP)} dlls")
from unipacker.core import Sample, UnpackerEngine, SimpleClient
sample=Sample('/tmp/mq.exe'); u=sample.unpacker
u.is_allowed=lambda a: not (TEXT_LO<=a<TEXT_HI)
ev=threading.Event()
engine=UnpackerEngine(sample,'/tmp/unp9.exe'); engine.register_client(SimpleClient(ev))
uc=engine.uc
uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED|UC_HOOK_MEM_WRITE_UNMAPPED|UC_HOOK_MEM_FETCH_UNMAPPED, lambda u,ac,ad,sz,v,x: patched_invalid(engine,u,ac,ad,sz,v,x))
_wcount=[0]
IATW={}
def watch_write(u,access,address,size,value,ud):
    IATW[address]=value
uc.hook_add(UC_HOOK_MEM_WRITE, watch_write, begin=0xc00000, end=0x1200000)
# map all DLLs into emulator memory
for nm,(base,size,img) in PREP.items():
    try:
        uc.mem_map(base,size); uc.mem_write(base,img); DLLMAP[nm]=(base,size,None)
    except Exception as e:
        print(f"  map fail {nm} @ {hex(base)}: {e}")
print(f"mapped {len(DLLMAP)} dlls into emulator")
# ---- RESUME FROM CHECKPOINT ----
try: uc.mem_unmap(0x0,0x100000)
except Exception: pass
import pickle
meta=pickle.load(open('/tmp/ckpt2/meta.pkl','rb'))
print("restoring", len(meta['regions']), "regions; ckpt cnt", meta['cnt'])
for (begin,size,perm) in meta['regions']:
    fn=f"/tmp/ckpt2/{begin:08x}_{size:08x}.bin"
    try: data=open(fn,'rb').read()
    except Exception as e: print("read fail",hex(begin),e); continue
    try: uc.mem_map(begin,size,perm)
    except Exception: pass
    try: uc.mem_write(begin,data)
    except Exception as e: print("write fail",hex(begin),e)
for r,v in meta['regs'].items():
    uc.reg_write(getattr(unicorn.x86_const,'UC_X86_REG_'+r), v)
ah=engine.apicall_handler
for k,v in meta['ah'].items(): setattr(ah,k,v)
cnt[0]=meta['cnt']
eip=meta['regs']['EIP']
print("resuming from eip",hex(eip),"esp",hex(meta['regs']['ESP']),"cnt",cnt[0])
def run():
    try: uc.emu_start(eip,0)
    except Exception as e: print("emu err",e)
    finally:
        for c in engine.clients: c.emu_done()
threading.Thread(target=run,daemon=True).start()
ev.wait(timeout=900); time.sleep(0.3)
print("FINAL cnt",cnt[0],"faults",faults[0],"captured IAT writes:",len(IATW))
import json as _json
# resolve using DLLMAP (mapped game DLLs) + UNIP, via export tables
import pefile as _pf, bisect as _bi, os as _os
RES=[]
for nm,(base,size,_) in list(DLLMAP.items()):
    path=_os.path.join('/tmp/dllset',nm+'.dll')
    if not _os.path.exists(path): continue
    try:
        pe=_pf.PE(path,fast_load=True); pe.parse_data_directories(directories=[_pf.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
    except Exception: continue
    exp={}
    if hasattr(pe,'DIRECTORY_ENTRY_EXPORT') and pe.DIRECTORY_ENTRY_EXPORT:
        for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            if e.address: exp[base+e.address]=(e.name.decode() if e.name else f"ord{e.ordinal}")
    RES.append((base,base+size,exp,nm))
# unipacker DLLs
for nm,b in [('kernel32',0x755D0000),('kernelbase',0x73D00000),('ntdll',0x77400000)]:
    p=f'/usr/local/lib/python3.11/dist-packages/unipacker/DLLs/{nm}.dll'
    pp={'kernel32':'kernel32','kernelbase':'KernelBase','ntdll':'ntdll'}[nm]
    p=f'/usr/local/lib/python3.11/dist-packages/unipacker/DLLs/{pp}.dll'
    if not _os.path.exists(p): continue
    try:
        pe=_pf.PE(p,fast_load=True); pe.parse_data_directories(directories=[_pf.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
    except Exception: continue
    exp={}
    if hasattr(pe,'DIRECTORY_ENTRY_EXPORT') and pe.DIRECTORY_ENTRY_EXPORT:
        for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            if e.address: exp[b+e.address]=(e.name.decode() if e.name else f"ord{e.ordinal}")
    RES.append((b,b+0x200000,exp,nm))
RES.sort(); STARTS=[r[0] for r in RES]
def rez(a):
    i=_bi.bisect_right(STARTS,a)-1
    if i<0: return None
    base,end,exp,nm=RES[i]
    return (nm,exp.get(a)) if base<=a<end else None
bydll={}
for slot,val in sorted(IATW.items()):
    r=rez(val)
    if r and r[1]: bydll.setdefault(r[0],[]).append((slot,r[1]))
tot=sum(len(v) for v in bydll.values())
print(f"RESOLVED {tot} imports across {len(bydll)} dlls:")
for d,l in sorted(bydll.items()): print(f"   {d}: {len(l)}")
_json.dump({hex(s):[hex(v)] for s,v in IATW.items()}, open('/tmp/iat_writes.json','w'))
_json.dump({d:[f for _,f in l] for d,l in bydll.items()}, open('/tmp/imports.json','w'))
print("saved /tmp/iat_writes.json + /tmp/imports.json")
