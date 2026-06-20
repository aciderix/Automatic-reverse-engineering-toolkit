import builtins, threading, sys, os, time, struct, collections, json
_ans=iter(['yes','ep','']); builtins.input=lambda p='': next(_ans,'')
import unicorn
from unicorn import *
from unicorn.x86_const import *
import unipacker.apicalls as A
import unipacker.core as C
from unipacker.utils import get_string
import pefile

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

def lean_code(self, uc, address, size, user_data):
    cnt[0]+=1; ring.append(address)
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
    if faults[0]<=30:
        sys.stdout.write(f"MEMINVALID#{faults[0]} acc={access} addr={hex(address)} eip={hex(uc.reg_read(UC_X86_REG_EIP))} cnt={cnt[0]}\n"); sys.stdout.flush()
    if faults[0]<200000:
        try: uc.mem_map(address&~0xfff,0x1000); return True
        except Exception: pass
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
# map all DLLs into emulator memory
for nm,(base,size,img) in PREP.items():
    try:
        uc.mem_map(base,size); uc.mem_write(base,img); DLLMAP[nm]=(base,size,None)
    except Exception as e:
        print(f"  map fail {nm} @ {hex(base)}: {e}")
print(f"mapped {len(DLLMAP)} dlls into emulator")
t=threading.Thread(target=engine.emu,daemon=True); t.start()
ev.wait(timeout=5000); time.sleep(0.3)
print("FINAL cnt",cnt[0],"faults",faults[0],"OEP",hex(oep[0]) if oep[0] else None)
