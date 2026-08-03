/* Heavy-form FLOOR (ASCII subset, sound): the 12 primitives rtlstr.c stands on, ported
 * once. NLS conversions on bytes 0-127 are exact (byte<->u16 identity); heap aliases the
 * host. This is what lets a whole Wine ntdll .c run without linking Wine. Non-ASCII/codepage
 * behaviour is OUT of this subset (would need the NLS tables) -> proven on ASCII only. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define NTAPI __attribute__((stdcall))

typedef long NTSTATUS; typedef uint16_t WCHAR; typedef unsigned long ULONG;
typedef unsigned char BOOLEAN; typedef unsigned short USHORT;
typedef struct { USHORT Length, MaximumLength; char *Buffer; } STRING;
typedef struct { USHORT Length, MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
#define OK 0
static WCHAR up(WCHAR c){ return (c>='a'&&c<='z')?c-32:c; }

NTSTATUS NTAPI RtlMultiByteToUnicodeN(WCHAR*d,ULONG dl,ULONG*rl,const char*s,ULONG sl){
    ULONG n=sl; if(n*2>dl)n=dl/2; for(ULONG i=0;i<n;i++)d[i]=(unsigned char)s[i];
    if(rl)*rl=n*2; return OK; }
NTSTATUS NTAPI RtlUnicodeToMultiByteN(char*d,ULONG dl,ULONG*rl,const WCHAR*s,ULONG sb){
    ULONG n=sb/2; if(n>dl)n=dl; for(ULONG i=0;i<n;i++)d[i]=(char)(s[i]&0xFF);
    if(rl)*rl=n; return OK; }
NTSTATUS NTAPI RtlUpcaseUnicodeToMultiByteN(char*d,ULONG dl,ULONG*rl,const WCHAR*s,ULONG sb){
    ULONG n=sb/2; if(n>dl)n=dl; for(ULONG i=0;i<n;i++)d[i]=(char)(up(s[i])&0xFF);
    if(rl)*rl=n; return OK; }
NTSTATUS NTAPI RtlOemToUnicodeN(WCHAR*d,ULONG dl,ULONG*rl,const char*s,ULONG sl){
    return RtlMultiByteToUnicodeN(d,dl,rl,s,sl); }
NTSTATUS NTAPI RtlUnicodeToOemN(char*d,ULONG dl,ULONG*rl,const WCHAR*s,ULONG sb){
    return RtlUnicodeToMultiByteN(d,dl,rl,s,sb); }
NTSTATUS NTAPI RtlUpcaseUnicodeToOemN(char*d,ULONG dl,ULONG*rl,const WCHAR*s,ULONG sb){
    return RtlUpcaseUnicodeToMultiByteN(d,dl,rl,s,sb); }
NTSTATUS NTAPI RtlMultiByteToUnicodeSize(ULONG*sz,const char*s,ULONG l){(void)s;*sz=l*2;return OK;}
NTSTATUS NTAPI RtlUnicodeToMultiByteSize(ULONG*sz,const WCHAR*s,ULONG b){(void)s;*sz=b/2;return OK;}
ULONG NTAPI RtlOemStringToUnicodeSize(const STRING*s){ return (s->Length+1)*sizeof(WCHAR); }
ULONG NTAPI RtlUnicodeStringToOemSize(const UNICODE_STRING*s){ return s->Length/sizeof(WCHAR)+1; }
long NTAPI RtlCompareUnicodeStrings(const WCHAR*s1,ULONG l1,const WCHAR*s2,ULONG l2,BOOLEAN ci){
    ULONG n=l1<l2?l1:l2; for(ULONG i=0;i<n;i++){ WCHAR a=s1[i],b=s2[i]; if(ci){a=up(a);b=up(b);}
        if(a!=b) return (long)a-(long)b; } return (long)l1-(long)l2; }
/* heap -> host */
void* NTAPI GetProcessHeap(void){ static char h; return &h; }
void* NTAPI RtlAllocateHeap(void*h,ULONG f,size_t n){(void)h;(void)f;return malloc(n);}
void* NTAPI RtlFreeHeap(void*h,ULONG f,void*p){(void)h;(void)f;free(p);return 0;}
/* 16-bit wide-string primitives: glibc's wcslen/wcschr assume 32-bit wchar_t, but Windows
 * WCHAR is 16-bit. On native these MUST be 16-bit-aware (ARET's HLE has aret_wcslen etc.). */
size_t wcslen(const uint16_t *s){ size_t n=0; while(s[n]) n++; return n; }
uint16_t *wcschr(const uint16_t *s, uint16_t c){ for(;;s++){ if(*s==c) return (uint16_t*)s; if(!*s) return 0; } }
