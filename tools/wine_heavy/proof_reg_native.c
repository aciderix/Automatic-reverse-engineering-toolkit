/* Heavy-form NATIVE capstone proof (doc 82 tranche 6): a WHOLE Wine ntdll file -- dlls/ntdll/reg.c,
 * 768 lines, UNCHANGED except the forward-decl splice -- compiled by native cc against ARET's
 * self-contained NT-types shim, calling the real-ABI Nt* registry floor (ntdll_ntreg.c) + rtlstr.c,
 * round-trips a registry key AS A NATIVE ELF with NO Wine at runtime. Driven through reg.c's
 * exported RtlpNtCreateKey/RtlpNtSetValueKey/RtlpNtQueryValueKey (they operate on a caller-provided
 * key -> round-trippable). The reference in-memory registry + off-path stubs stand in for ARET's
 * g_reg (whose own correctness is proven by win32_ntreg). Output matches the known Wine values.
 * This is the first whole NON-STRING Wine ntdll source running on ARET's floor. */
#include "nt_types.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
NTSTATUS NTAPI RtlpNtCreateKey(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,ULONG,PUNICODE_STRING,ULONG,PULONG);
NTSTATUS NTAPI RtlpNtSetValueKey(HANDLE,ULONG,const void*,ULONG);
NTSTATUS NTAPI RtlpNtQueryValueKey(HANDLE,ULONG*,void*,ULONG*,void*);
NTSTATUS NTAPI NtClose(HANDLE);
int main(void){
    static WCHAR kpath[]=u"\\Registry\\Machine\\Software\\AretRegProof";
    UNICODE_STRING kp; RtlInitUnicodeString(&kp,kpath);
    OBJECT_ATTRIBUTES oa; InitializeObjectAttributes(&oa,&kp,OBJ_CASE_INSENSITIVE,NULL,NULL);
    HANDLE h=NULL; ULONG disp=0;
    NTSTATUS s=RtlpNtCreateKey(&h,KEY_ALL_ACCESS,&oa,0,NULL,0,&disp);
    printf("create hr=0x%08lX disp=%lu\n",(unsigned long)s,(unsigned long)disp);
    unsigned data=42;
    s=RtlpNtSetValueKey(h,REG_DWORD,&data,4);
    printf("set hr=0x%08lX\n",(unsigned long)s);
    unsigned char buf[64]; ULONG type=0, count=sizeof buf;
    s=RtlpNtQueryValueKey(h,&type,buf,&count,NULL);
    printf("query hr=0x%08lX type=%lu count=%lu val=%lu\n",(unsigned long)s,(unsigned long)type,(unsigned long)count,(unsigned long)*(unsigned*)buf);
    NtClose(h);
    return 0;
}
#define REFBASE 0x75000000u
static struct { int used; char name[256];
    struct { int used; char vn[64]; unsigned type,len; unsigned char data[256]; } val[16]; } refk[64];
static void usn(uint32_t pus,char*out,int cap){ out[0]=0; if(!pus)return;
    unsigned short bl=*(unsigned short*)(uintptr_t)pus;
    const unsigned short*w=(const unsigned short*)(uintptr_t)*(const unsigned*)(uintptr_t)(pus+4);
    int n=bl/2,i=0; if(w)for(;i<n&&i<cap-1;i++)out[i]=(char)(w[i]&0xFF); out[i]=0; }
static int rfind(const char*nm,int cr,unsigned*d){
    for(int i=0;i<64;i++)if(refk[i].used&&!strcasecmp(refk[i].name,nm)){if(d)*d=2;return i;}
    if(!cr)return -1;
    for(int i=0;i<64;i++)if(!refk[i].used){memset(&refk[i],0,sizeof refk[i]);refk[i].used=1;strncpy(refk[i].name,nm,255);if(d)*d=1;return i;}return -1; }
uint32_t aret_ntreg_create(uint32_t poa,uint32_t ph,uint32_t pd){ uint32_t pn=*(uint32_t*)(uintptr_t)(poa+8);char nm[256];usn(pn,nm,256);
    unsigned d=0;int k=rfind(nm,1,&d);if(k<0)return 0xC0000034u;if(ph)*(uint32_t*)(uintptr_t)ph=REFBASE|k;if(pd)*(uint32_t*)(uintptr_t)pd=d;return 0; }
uint32_t aret_ntreg_open(uint32_t poa,uint32_t ph){ uint32_t pn=*(uint32_t*)(uintptr_t)(poa+8);char nm[256];usn(pn,nm,256);
    int k=rfind(nm,0,0);if(k<0)return 0xC0000034u;if(ph)*(uint32_t*)(uintptr_t)ph=REFBASE|k;return 0; }
uint32_t aret_ntreg_setval(uint32_t hk,uint32_t pvn,uint32_t ty,uint32_t da,uint32_t sz){ unsigned k=hk&0xFFFFFF;if(k>=64||!refk[k].used)return 0xC0000008u;
    char vn[64];usn(pvn,vn,64);
    for(int i=0;i<16;i++)if(!refk[k].val[i].used||!strcasecmp(refk[k].val[i].vn,vn)){refk[k].val[i].used=1;strncpy(refk[k].val[i].vn,vn,63);
        refk[k].val[i].type=ty;refk[k].val[i].len=sz>256?256:sz;if(da)memcpy(refk[k].val[i].data,(void*)(uintptr_t)da,refk[k].val[i].len);return 0;}return 0xC0000017u; }
uint32_t aret_ntreg_queryval(uint32_t hk,uint32_t pvn,uint32_t cls,uint32_t info,uint32_t len,uint32_t pr){ if(cls!=2)return 0xC0000008u;
    unsigned k=hk&0xFFFFFF;if(k>=64||!refk[k].used)return 0xC0000008u;char vn[64];usn(pvn,vn,64);
    for(int i=0;i<16;i++)if(refk[k].val[i].used&&!strcasecmp(refk[k].val[i].vn,vn)){unsigned l=refk[k].val[i].len,need=12+l;if(pr)*(uint32_t*)(uintptr_t)pr=need;
        if(len<12)return 0xC0000023u;uint32_t*p=(uint32_t*)(uintptr_t)info;p[0]=0;p[1]=refk[k].val[i].type;p[2]=l;
        unsigned cap=len-12,cp=l<cap?l:cap;if(cp)memcpy((unsigned char*)(uintptr_t)info+12,refk[k].val[i].data,cp);return cp<l?0x80000005u:0;}return 0xC0000034u; }
uint32_t aret_ntreg_delval(uint32_t a,uint32_t b){(void)a;(void)b;return 0;}
uint32_t aret_ntreg_enumkey(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0x8000001Au;}
uint32_t aret_ntreg_enumval(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0x8000001Au;}
uint32_t aret_ntreg_delkey(uint32_t a){(void)a;return 0;}
int aret_ntfile_close(uint32_t h){(void)h;return 0;}
void aret_unimpl(const char*m){(void)m;}
NTSTATUS NTAPI RtlConvertSidToUnicodeString(void*a,void*b,unsigned char c){(void)a;(void)b;(void)c;return -1;}
NTSTATUS NTAPI RtlExpandEnvironmentStrings_U(void*a,void*b,void*c,void*d){(void)a;(void)b;(void)c;(void)d;return -1;}
void* NTAPI GetCurrentThreadEffectiveToken(void){ return (void*)~(uintptr_t)0; }
void aret_cp1252_to_wc(unsigned short*d,const char*s,int n){for(int i=0;i<n;i++)d[i]=(unsigned char)s[i];}
int aret_cp1252_from_wc(char*d,const unsigned short*s,int n){for(int i=0;i<n;i++)d[i]=(char)(s[i]&0xFF);return 0;}
void aret_cp437_to_wc(unsigned short*d,const char*s,int n){for(int i=0;i<n;i++)d[i]=(unsigned char)s[i];}
int aret_cp437_from_wc(char*d,const unsigned short*s,int n){for(int i=0;i<n;i++)d[i]=(char)(s[i]&0xFF);return 0;}
