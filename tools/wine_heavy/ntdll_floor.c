/* Heavy-form FLOOR (ASCII subset, sound): the 12 primitives rtlstr.c stands on, ported
 * once. NLS conversions on bytes 0-127 are exact (byte<->u16 identity); heap aliases the
 * host. This is what lets a whole Wine ntdll .c run without linking Wine. Non-ASCII/codepage
 * behaviour is OUT of this subset (would need the NLS tables) -> proven on ASCII only. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define NTAPI __attribute__((stdcall))
extern void aret_unimpl(const char *);

typedef long NTSTATUS; typedef uint16_t WCHAR; typedef unsigned long ULONG;
typedef unsigned char BOOLEAN; typedef unsigned short USHORT;
typedef struct { USHORT Length, MaximumLength; char *Buffer; } STRING;
typedef struct { USHORT Length, MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
#define OK 0
static WCHAR up(WCHAR c){ return (c>='a'&&c<='z')?c-32:c; }
/* SOUND boundary (§0): the ASCII floor maps 0-127 exactly (identity). Bytes/units >127 are
 * codepage-dependent (CP-1252 != Latin-1) and NOT modelled here -> abort loudly rather than
 * emit a Latin-1 guess that silently diverges from Wine. Reached only if a program actually
 * converts non-ASCII text; the NLS-table port lifts this. */
static void ascii_only_b(const char*s,ULONG n){ for(ULONG i=0;i<n;i++) if((unsigned char)s[i]>0x7f) aret_unimpl("ntdll floor: non-ASCII codepage conversion (NLS tables not modelled)"); }
static void ascii_only_w(const WCHAR*s,ULONG n){ for(ULONG i=0;i<n;i++) if(s[i]>0x7f) aret_unimpl("ntdll floor: non-ASCII codepage conversion (NLS tables not modelled)"); }
/* Shared ANSI(CP1252)->UTF16, provided by the HLE (aret_win32.c) — the single conversion the
 * whole system uses. Full CP1252, bit-identical Wine on 0x00-0xFF (the ANSI-forward direction). */
extern void aret_cp1252_to_wc(WCHAR *dst, const char *src, int n);
extern int aret_cp1252_from_wc(char *dst, const WCHAR *src, int n);
extern void aret_cp437_to_wc(WCHAR *dst, const char *src, int n);   /* OEM forward */
extern int aret_cp437_from_wc(char *dst, const WCHAR *src, int n);  /* OEM reverse */

NTSTATUS NTAPI RtlMultiByteToUnicodeN(WCHAR*d,ULONG dl,ULONG*rl,const char*s,ULONG sl){
    ULONG n=sl; if(n*2>dl)n=dl/2; aret_cp1252_to_wc(d,s,(int)n);
    if(rl)*rl=n*2; return OK; }
/* Reverse UTF16->ANSI(CP1252): full CP1252 + Wine best-fit via the shared aret_cp1252_from_wc. */
NTSTATUS NTAPI RtlUnicodeToMultiByteN(char*d,ULONG dl,ULONG*rl,const WCHAR*s,ULONG sb){
    ULONG n=sb/2; if(n>dl)n=dl; aret_cp1252_from_wc(d,s,(int)n);
    if(rl)*rl=n; return OK; }
/* OEM(CP437) forward & reverse: full CP437 + Wine best-fit via the shared aret_cp437_*. */
NTSTATUS NTAPI RtlOemToUnicodeN(WCHAR*d,ULONG dl,ULONG*rl,const char*s,ULONG sl){
    ULONG n=sl; if(n*2>dl)n=dl/2; aret_cp437_to_wc(d,s,(int)n);
    if(rl)*rl=n*2; return OK; }
NTSTATUS NTAPI RtlUnicodeToOemN(char*d,ULONG dl,ULONG*rl,const WCHAR*s,ULONG sb){
    ULONG n=sb/2; if(n>dl)n=dl; aret_cp437_from_wc(d,s,(int)n);
    if(rl)*rl=n; return OK; }
/* Upcase-reverse variants need the Unicode upcase table (not modelled beyond ASCII) -> stay
 * ASCII-exact + sound abort; a follow-up lifts them. */
NTSTATUS NTAPI RtlUpcaseUnicodeToMultiByteN(char*d,ULONG dl,ULONG*rl,const WCHAR*s,ULONG sb){
    ULONG n=sb/2; if(n>dl)n=dl; ascii_only_w(s,n); for(ULONG i=0;i<n;i++)d[i]=(char)(up(s[i])&0xFF);
    if(rl)*rl=n; return OK; }
NTSTATUS NTAPI RtlUpcaseUnicodeToOemN(char*d,ULONG dl,ULONG*rl,const WCHAR*s,ULONG sb){
    ULONG n=sb/2; if(n>dl)n=dl; ascii_only_w(s,n); for(ULONG i=0;i<n;i++)d[i]=(char)(up(s[i])&0xFF);
    if(rl)*rl=n; return OK; }
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
/* _snwprintf_s: pulled in by RtlFormatMessage (untested path). Not modelled -> sound abort
 * if ever reached (never on the string paths). WEAK so a real CRT (mingw) or a proof driver
 * overrides it; provides the symbol so the link closes without a guessed body. */
__attribute__((weak)) int _snwprintf_s(uint16_t *b, size_t n, size_t c, const uint16_t *f, ...) {
    (void)b; (void)n; (void)c; (void)f; aret_unimpl("_snwprintf_s (ntdll RtlFormatMessage)"); return -1;
}
