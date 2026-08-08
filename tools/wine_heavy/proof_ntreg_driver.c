/* Heavy-form PROOF driver for the real-ABI Nt* registry floor (doc 82 tranche 5). It calls the
 * NT registry syscalls the way a compiled Wine ntdll .c would (real NTAPI, real args), then prints
 * the result. Linked two ways by proof_ntreg.sh:
 *   ours   = this driver + ntdll_ntreg.o (our floor wrappers) + the reference core below;
 *   oracle = this driver + Wine's real ntdll (-lntdll).
 * Bit-identical output => our floor wrappers implement the NT registry ABI correctly. The
 * reference core is a minimal in-memory registry standing in for ARET's g_reg (whose own
 * correctness is proven separately by the win32_ntreg winediff fixture, sharing the same cores). */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>

NTSTATUS WINAPI NtCreateKey(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,ULONG,PUNICODE_STRING,ULONG,PULONG);
NTSTATUS WINAPI NtOpenKey(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES);
NTSTATUS WINAPI NtSetValueKey(HANDLE,PUNICODE_STRING,ULONG,ULONG,PVOID,ULONG);
NTSTATUS WINAPI NtQueryValueKey(HANDLE,PUNICODE_STRING,int,PVOID,ULONG,PULONG);
NTSTATUS WINAPI NtClose(HANDLE);
typedef struct { ULONG TitleIndex, Type, DataLength; UCHAR Data[1]; } KVPI;

int main(void){
    UNICODE_STRING kp; RtlInitUnicodeString(&kp, L"\\Registry\\Machine\\Software\\AretProof");
    OBJECT_ATTRIBUTES oa; InitializeObjectAttributes(&oa,&kp,OBJ_CASE_INSENSITIVE,NULL,NULL);
    HANDLE h=NULL; ULONG disp=0;
    NTSTATUS s=NtCreateKey(&h,KEY_ALL_ACCESS,&oa,0,NULL,0,&disp);
    printf("create hr=0x%08lX disp=%lu\n",(unsigned long)s,(unsigned long)disp);

    UNICODE_STRING vn; RtlInitUnicodeString(&vn,L"Answer");
    DWORD data=42;
    s=NtSetValueKey(h,&vn,0,REG_DWORD,&data,sizeof data);
    printf("set hr=0x%08lX\n",(unsigned long)s);

    UCHAR buf[64]; ULONG rl=0;
    s=NtQueryValueKey(h,&vn,2,buf,sizeof buf,&rl);
    KVPI *pi=(void*)buf;
    printf("query hr=0x%08lX rl=%lu type=%lu dlen=%lu val=%lu\n",
           (unsigned long)s,(unsigned long)rl,(unsigned long)pi->Type,
           (unsigned long)pi->DataLength,(unsigned long)*(DWORD*)pi->Data);

    /* too-small buffer -> BUFFER_OVERFLOW + ResultLength */
    ULONG rl2=0; s=NtQueryValueKey(h,&vn,2,buf,13,&rl2);
    printf("small hr=0x%08lX rl=%lu\n",(unsigned long)s,(unsigned long)rl2);

    /* re-open by name, read again */
    HANDLE h2=NULL; s=NtOpenKey(&h2,KEY_READ,&oa);
    ULONG rl3=0; NtQueryValueKey(h2,&vn,2,buf,sizeof buf,&rl3);
    printf("reopen hr=0x%08lX val=%lu\n",(unsigned long)s,(unsigned long)*(DWORD*)((KVPI*)buf)->Data);
    NtClose(h); NtClose(h2);
    return 0;
}

#ifdef ARET_REFERENCE_CORE
/* -------- reference in-memory registry (only compiled into `ours`): stands in for ARET's g_reg,
 * implementing the exact contract the floor wrappers expect. Absolute names are the key identity
 * (the driver uses \Registry\Machine\... paths). -------- */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
/* RtlInitUnicodeString normally comes from ntdll; `ours` does not link ntdll, so provide it. */
VOID WINAPI RtlInitUnicodeString(PUNICODE_STRING d, PCWSTR s){
    size_t n=0; if(s) while(s[n]) n++;
    d->Length=(unsigned short)(n*2); d->MaximumLength=(unsigned short)(n*2+2); d->Buffer=(PWSTR)s;
}
#define REFBASE 0x75000000u
static struct { int used; char name[256];
                struct { int used; char vn[64]; unsigned type, len; unsigned char data[256]; } val[16];
              } refk[64];
static void us_narrow(uint32_t pus, char *out, int cap){
    out[0]=0; if(!pus) return;
    unsigned short blen=*(unsigned short*)(uintptr_t)pus;
    const unsigned short *w=(const unsigned short*)(uintptr_t)*(const unsigned*)(uintptr_t)(pus+4);
    int n=blen/2,i=0; if(w) for(;i<n&&i<cap-1;i++) out[i]=(char)(w[i]&0xFF); out[i]=0;
}
static int ref_find(const char*name,int create,unsigned*disp){
    for(int i=0;i<64;i++) if(refk[i].used && !strcasecmp(refk[i].name,name)){ if(disp)*disp=2; return i; }
    if(!create) return -1;
    for(int i=0;i<64;i++) if(!refk[i].used){ memset(&refk[i],0,sizeof refk[i]); refk[i].used=1;
        strncpy(refk[i].name,name,255); if(disp)*disp=1; return i; }
    return -1;
}
uint32_t aret_ntreg_create(uint32_t poa,uint32_t phkey,uint32_t pdisp){
    uint32_t pname=*(uint32_t*)(uintptr_t)(poa+8); char nm[256]; us_narrow(pname,nm,sizeof nm);
    unsigned disp=0; int k=ref_find(nm,1,&disp); if(k<0) return 0xC0000034u;
    if(phkey)*(uint32_t*)(uintptr_t)phkey=REFBASE|(unsigned)k; if(pdisp)*(uint32_t*)(uintptr_t)pdisp=disp;
    return 0;
}
uint32_t aret_ntreg_open(uint32_t poa,uint32_t phkey){
    uint32_t pname=*(uint32_t*)(uintptr_t)(poa+8); char nm[256]; us_narrow(pname,nm,sizeof nm);
    int k=ref_find(nm,0,0); if(k<0) return 0xC0000034u;
    if(phkey)*(uint32_t*)(uintptr_t)phkey=REFBASE|(unsigned)k; return 0;
}
uint32_t aret_ntreg_setval(uint32_t hkey,uint32_t pvn,uint32_t type,uint32_t data,uint32_t size){
    unsigned k=hkey&0xFFFFFF; if(k>=64||!refk[k].used) return 0xC0000008u;
    char vn[64]; us_narrow(pvn,vn,sizeof vn);
    for(int i=0;i<16;i++) if(refk[k].val[i].used && !strcasecmp(refk[k].val[i].vn,vn)){
        refk[k].val[i].type=type; refk[k].val[i].len=size>256?256:size;
        if(data) memcpy(refk[k].val[i].data,(void*)(uintptr_t)data,refk[k].val[i].len); return 0; }
    for(int i=0;i<16;i++) if(!refk[k].val[i].used){ refk[k].val[i].used=1; strncpy(refk[k].val[i].vn,vn,63);
        refk[k].val[i].type=type; refk[k].val[i].len=size>256?256:size;
        if(data) memcpy(refk[k].val[i].data,(void*)(uintptr_t)data,refk[k].val[i].len); return 0; }
    return 0xC0000017u;
}
uint32_t aret_ntreg_queryval(uint32_t hkey,uint32_t pvn,uint32_t cls,uint32_t info,uint32_t length,uint32_t presult){
    if(cls!=2) return 0xC0000008u;
    unsigned k=hkey&0xFFFFFF; if(k>=64||!refk[k].used) return 0xC0000008u;
    char vn[64]; us_narrow(pvn,vn,sizeof vn);
    for(int i=0;i<16;i++) if(refk[k].val[i].used && !strcasecmp(refk[k].val[i].vn,vn)){
        unsigned len=refk[k].val[i].len, need=12+len;
        if(presult)*(uint32_t*)(uintptr_t)presult=need;
        if(length<12) return 0xC0000023u;
        uint32_t*p=(uint32_t*)(uintptr_t)info; p[0]=0; p[1]=refk[k].val[i].type; p[2]=len;
        unsigned cap=length-12, copy=len<cap?len:cap; if(copy) memcpy((unsigned char*)(uintptr_t)info+12,refk[k].val[i].data,copy);
        return copy<len?0x80000005u:0; }
    return 0xC0000034u;
}
uint32_t aret_ntreg_delval(uint32_t hkey,uint32_t pvn){
    unsigned k=hkey&0xFFFFFF; if(k>=64||!refk[k].used) return 0xC0000008u;
    char vn[64]; us_narrow(pvn,vn,sizeof vn);
    for(int i=0;i<16;i++) if(refk[k].val[i].used && !strcasecmp(refk[k].val[i].vn,vn)){ refk[k].val[i].used=0; return 0; }
    return 0xC0000034u;
}
int aret_ntfile_close(uint32_t h){ (void)h; return 0; }   /* keys persist; handle close is a no-op */
/* enum/delete are referenced by ntdll_ntreg.c but not exercised by this driver -> link stubs. */
uint32_t aret_ntreg_enumkey(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0x8000001Au;}
uint32_t aret_ntreg_enumval(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0x8000001Au;}
uint32_t aret_ntreg_delkey(uint32_t a){(void)a;return 0;}
void aret_unimpl(const char*m){(void)m;}
#endif /* ARET_REFERENCE_CORE */
