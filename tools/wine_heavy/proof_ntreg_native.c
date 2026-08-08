/* Heavy-form NATIVE proof driver for the real-ABI Nt* registry floor (doc 82 tranche 6). Same
 * idea as proof_ntreg_driver.c but through ARET's REAL build model: native cc (Linux/glibc), no
 * windows.h, -fshort-wchar so L"" is 16-bit like Windows WCHAR. Compiled + linked with our floor
 * wrappers (ntdll_ntreg.c) and a reference in-memory registry, it runs as a native ELF (no Wine
 * at runtime) and prints values checked against the known-correct output. Self-contained. */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

#define NTAPI __attribute__((stdcall))
typedef long NTSTATUS; typedef uint16_t WCHAR; typedef unsigned long ULONG; typedef void *HANDLE;
typedef unsigned long ACCESS_MASK;
typedef struct { unsigned short Length, MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
typedef struct { ULONG Length; HANDLE RootDirectory; UNICODE_STRING *ObjectName;
                 ULONG Attributes; void *SecurityDescriptor, *SecurityQualityOfService; } OBJECT_ATTRIBUTES;

/* the floor wrappers (ntdll_ntreg.c) */
NTSTATUS NTAPI NtCreateKey(HANDLE*,ACCESS_MASK,OBJECT_ATTRIBUTES*,ULONG,UNICODE_STRING*,ULONG,ULONG*);
NTSTATUS NTAPI NtOpenKey(HANDLE*,ACCESS_MASK,OBJECT_ATTRIBUTES*);
NTSTATUS NTAPI NtSetValueKey(HANDLE,UNICODE_STRING*,ULONG,ULONG,void*,ULONG);
NTSTATUS NTAPI NtQueryValueKey(HANDLE,UNICODE_STRING*,int,void*,ULONG,ULONG*);
NTSTATUS NTAPI NtClose(HANDLE);

static void initus(UNICODE_STRING *u, WCHAR *s){ ULONG n=0; while(s[n]) n++; u->Length=n*2; u->MaximumLength=n*2+2; u->Buffer=s; }

int main(void){
    static WCHAR kpath[]=u"\\Registry\\Machine\\Software\\AretProof";
    static WCHAR vname[]=u"Answer";
    UNICODE_STRING kp; initus(&kp,kpath);
    OBJECT_ATTRIBUTES oa; memset(&oa,0,sizeof oa); oa.Length=sizeof oa; oa.ObjectName=&kp; oa.Attributes=0x40;
    HANDLE h=0; ULONG disp=0;
    NTSTATUS s=NtCreateKey(&h,0xF003F,&oa,0,0,0,&disp);
    printf("create hr=0x%08lX disp=%lu\n",(unsigned long)s,(unsigned long)disp);
    UNICODE_STRING vn; initus(&vn,vname);
    unsigned data=42;
    s=NtSetValueKey(h,&vn,0,4/*REG_DWORD*/,&data,4);
    printf("set hr=0x%08lX\n",(unsigned long)s);
    unsigned char buf[64]; ULONG rl=0;
    s=NtQueryValueKey(h,&vn,2,buf,sizeof buf,&rl);
    printf("query hr=0x%08lX rl=%lu type=%lu dlen=%lu val=%lu\n",(unsigned long)s,(unsigned long)rl,
        (unsigned long)*(unsigned*)(buf+4),(unsigned long)*(unsigned*)(buf+8),(unsigned long)*(unsigned*)(buf+12));
    ULONG rl2=0; s=NtQueryValueKey(h,&vn,2,buf,13,&rl2);
    printf("small hr=0x%08lX rl=%lu\n",(unsigned long)s,(unsigned long)rl2);
    HANDLE h2=0; s=NtOpenKey(&h2,0x20019,&oa);
    ULONG rl3=0; NtQueryValueKey(h2,&vn,2,buf,sizeof buf,&rl3);
    printf("reopen hr=0x%08lX val=%lu\n",(unsigned long)s,(unsigned long)*(unsigned*)(buf+12));
    NtClose(h); NtClose(h2);
    return 0;
}

/* -------- reference in-memory registry (stands in for ARET's g_reg) -------- */
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
    if(phkey)*(uint32_t*)(uintptr_t)phkey=REFBASE|(unsigned)k; if(pdisp)*(uint32_t*)(uintptr_t)pdisp=disp; return 0;
}
uint32_t aret_ntreg_open(uint32_t poa,uint32_t phkey){
    uint32_t pname=*(uint32_t*)(uintptr_t)(poa+8); char nm[256]; us_narrow(pname,nm,sizeof nm);
    int k=ref_find(nm,0,0); if(k<0) return 0xC0000034u;
    if(phkey)*(uint32_t*)(uintptr_t)phkey=REFBASE|(unsigned)k; return 0;
}
uint32_t aret_ntreg_setval(uint32_t hkey,uint32_t pvn,uint32_t type,uint32_t data,uint32_t size){
    unsigned k=hkey&0xFFFFFF; if(k>=64||!refk[k].used) return 0xC0000008u;
    char vn[64]; us_narrow(pvn,vn,sizeof vn);
    for(int i=0;i<16;i++) if(!refk[k].val[i].used || !strcasecmp(refk[k].val[i].vn,vn)){
        refk[k].val[i].used=1; strncpy(refk[k].val[i].vn,vn,63); refk[k].val[i].type=type;
        refk[k].val[i].len=size>256?256:size; if(data) memcpy(refk[k].val[i].data,(void*)(uintptr_t)data,refk[k].val[i].len); return 0; }
    return 0xC0000017u;
}
uint32_t aret_ntreg_queryval(uint32_t hkey,uint32_t pvn,uint32_t cls,uint32_t info,uint32_t length,uint32_t presult){
    if(cls!=2) return 0xC0000008u;
    unsigned k=hkey&0xFFFFFF; if(k>=64||!refk[k].used) return 0xC0000008u;
    char vn[64]; us_narrow(pvn,vn,sizeof vn);
    for(int i=0;i<16;i++) if(refk[k].val[i].used && !strcasecmp(refk[k].val[i].vn,vn)){
        unsigned len=refk[k].val[i].len, need=12+len; if(presult)*(uint32_t*)(uintptr_t)presult=need;
        if(length<12) return 0xC0000023u;
        uint32_t*p=(uint32_t*)(uintptr_t)info; p[0]=0; p[1]=refk[k].val[i].type; p[2]=len;
        unsigned cap=length-12, copy=len<cap?len:cap; if(copy) memcpy((unsigned char*)(uintptr_t)info+12,refk[k].val[i].data,copy);
        return copy<len?0x80000005u:0; }
    return 0xC0000034u;
}
uint32_t aret_ntreg_delval(uint32_t hkey,uint32_t pvn){ (void)hkey;(void)pvn; return 0; }
int aret_ntfile_close(uint32_t h){ (void)h; return 0; }
/* enum/delete referenced by ntdll_ntreg.c but not exercised here -> link stubs. */
uint32_t aret_ntreg_enumkey(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0x8000001Au;}
uint32_t aret_ntreg_enumval(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0x8000001Au;}
uint32_t aret_ntreg_delkey(uint32_t a){(void)a;return 0;}
void aret_unimpl(const char*m){(void)m;}
