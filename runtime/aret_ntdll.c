/* Heavy-form ADAPTERS (doc 82): route ntdll Rtl* imports to the REAL Wine bodies compiled
 * from runtime/wine_heavy/rtlstr.c. Each aret_Rtl*(esp) unpacks the stdcall args off the
 * guest stack and calls the compiled-in Rtl* with the guest pointers directly — ARET maps
 * guest memory 1:1 in the low 32-bit range, so a guest address IS a valid host pointer, the
 * same contract every other HLE shim uses. The Rtl* bodies stand on the ASCII floor in
 * runtime/wine_heavy/ntdll_floor.c; behaviour is bit-identical to Wine on the ASCII subset
 * (proven: tools/wine_heavy/proof_native.sh), and the floor aborts sound beyond it.
 *
 * This file compiles with the STANDARD HLE flags (no -fshort-wchar): it treats every wide
 * string as an opaque guest pointer and never dereferences wide data itself, so the WCHAR
 * width of the compiled Wine object (16-bit, -fshort-wchar) never crosses this boundary.
 *
 * The Wine bodies are compiled and linked only on native i386 (src/builder/mod.rs gates the
 * -fshort-wchar compile there). Everywhere else (64-bit, wasm) there is nothing to call, so
 * the adapters fall back to a sound abort rather than an undefined symbol. */
#include <stdint.h>
void aret_unimpl(const char *);

#define NA(i) (((const uint32_t *)(uintptr_t)esp)[i])
#define NP(i) ((void *)(uintptr_t)NA(i))
#define NTAPI __attribute__((stdcall))

#if defined(__i386__) && !defined(__wasm__)

/* Real-ABI prototypes of the Wine bodies (see runtime/wine_heavy/rtlstr.c). STRING /
 * UNICODE_STRING and wide buffers are opaque here — passed straight through as guest ptrs. */
void     NTAPI RtlInitString(void *, const char *);
void     NTAPI RtlInitAnsiString(void *, const char *);
void     NTAPI RtlInitUnicodeString(void *, const void *);
long     NTAPI RtlAnsiStringToUnicodeString(void *, const void *, unsigned char);
long     NTAPI RtlUnicodeStringToAnsiString(void *, const void *, unsigned char);
long     NTAPI RtlOemStringToUnicodeString(void *, const void *, unsigned char);
long     NTAPI RtlUnicodeStringToOemString(void *, const void *, unsigned char);
void     NTAPI RtlFreeAnsiString(void *);
void     NTAPI RtlFreeUnicodeString(void *);
void     NTAPI RtlFreeOemString(void *);
unsigned char NTAPI RtlEqualString(const void *, const void *, unsigned char);
unsigned char NTAPI RtlEqualUnicodeString(const void *, const void *, unsigned char);
long     NTAPI RtlCompareString(const void *, const void *, unsigned char);
long     NTAPI RtlCompareUnicodeString(const void *, const void *, unsigned char);
long     NTAPI RtlIntegerToChar(uint32_t, uint32_t, uint32_t, char *);
long     NTAPI RtlIntegerToUnicodeString(uint32_t, uint32_t, void *);
long     NTAPI RtlCharToInteger(const char *, uint32_t, uint32_t *);
long     NTAPI RtlUnicodeStringToInteger(const void *, uint32_t, uint32_t *);
unsigned char NTAPI RtlCreateUnicodeString(void *, const void *);
unsigned char NTAPI RtlCreateUnicodeStringFromAsciiz(void *, const char *);
long     NTAPI RtlAppendUnicodeStringToString(void *, const void *);
long     NTAPI RtlAppendUnicodeToString(void *, const void *);
unsigned char NTAPI RtlPrefixString(const void *, const void *, unsigned char);
/* Registry Rtlp* wrappers (the whole Wine reg.c, runtime/wine_heavy/reg.c): thin exported
 * wrappers over the Nt* registry syscalls, which resolve to the real-ABI floor (ntdll_ntreg.c ->
 * ARET's g_reg). They operate on a caller-provided key, so a PE round-trips through compiled Wine. */
long NTAPI RtlpNtCreateKey(void *, uint32_t, const void *, uint32_t, const void *, uint32_t, void *);
long NTAPI RtlpNtOpenKey(void *, uint32_t, const void *);
long NTAPI RtlpNtSetValueKey(void *, uint32_t, const void *, uint32_t);
long NTAPI RtlpNtQueryValueKey(void *, void *, void *, void *, void *);
/* NLS conversions (the floor, runtime/wine_heavy/ntdll_floor.c) — apps import these directly too. */
long NTAPI RtlMultiByteToUnicodeN(void *, unsigned long, unsigned long *, const void *, unsigned long);
long NTAPI RtlUnicodeToMultiByteN(void *, unsigned long, unsigned long *, const void *, unsigned long);
long NTAPI RtlUpcaseUnicodeToMultiByteN(void *, unsigned long, unsigned long *, const void *, unsigned long);
long NTAPI RtlOemToUnicodeN(void *, unsigned long, unsigned long *, const void *, unsigned long);
long NTAPI RtlUnicodeToOemN(void *, unsigned long, unsigned long *, const void *, unsigned long);
long NTAPI RtlUpcaseUnicodeToOemN(void *, unsigned long, unsigned long *, const void *, unsigned long);
long NTAPI RtlMultiByteToUnicodeSize(unsigned long *, const void *, unsigned long);
long NTAPI RtlUnicodeToMultiByteSize(unsigned long *, const void *, unsigned long);
unsigned long NTAPI RtlOemStringToUnicodeSize(const void *);
unsigned long NTAPI RtlUnicodeStringToOemSize(const void *);

uint32_t aret_RtlMultiByteToUnicodeN(uint32_t esp) { return (uint32_t)RtlMultiByteToUnicodeN(NP(0), NA(1), (unsigned long *)NP(2), NP(3), NA(4)); }
uint32_t aret_RtlUnicodeToMultiByteN(uint32_t esp) { return (uint32_t)RtlUnicodeToMultiByteN(NP(0), NA(1), (unsigned long *)NP(2), NP(3), NA(4)); }
uint32_t aret_RtlUpcaseUnicodeToMultiByteN(uint32_t esp) { return (uint32_t)RtlUpcaseUnicodeToMultiByteN(NP(0), NA(1), (unsigned long *)NP(2), NP(3), NA(4)); }
uint32_t aret_RtlOemToUnicodeN(uint32_t esp) { return (uint32_t)RtlOemToUnicodeN(NP(0), NA(1), (unsigned long *)NP(2), NP(3), NA(4)); }
uint32_t aret_RtlUnicodeToOemN(uint32_t esp) { return (uint32_t)RtlUnicodeToOemN(NP(0), NA(1), (unsigned long *)NP(2), NP(3), NA(4)); }
uint32_t aret_RtlUpcaseUnicodeToOemN(uint32_t esp) { return (uint32_t)RtlUpcaseUnicodeToOemN(NP(0), NA(1), (unsigned long *)NP(2), NP(3), NA(4)); }
uint32_t aret_RtlMultiByteToUnicodeSize(uint32_t esp) { return (uint32_t)RtlMultiByteToUnicodeSize((unsigned long *)NP(0), NP(1), NA(2)); }
uint32_t aret_RtlUnicodeToMultiByteSize(uint32_t esp) { return (uint32_t)RtlUnicodeToMultiByteSize((unsigned long *)NP(0), NP(1), NA(2)); }
uint32_t aret_RtlOemStringToUnicodeSize(uint32_t esp) { return (uint32_t)RtlOemStringToUnicodeSize(NP(0)); }
uint32_t aret_RtlUnicodeStringToOemSize(uint32_t esp) { return (uint32_t)RtlUnicodeStringToOemSize(NP(0)); }

uint32_t aret_RtlInitString(uint32_t esp)  { RtlInitString(NP(0), (const char *)NP(1)); return 0; }
uint32_t aret_RtlInitAnsiString(uint32_t esp) { RtlInitAnsiString(NP(0), (const char *)NP(1)); return 0; }
uint32_t aret_RtlInitUnicodeString(uint32_t esp) { RtlInitUnicodeString(NP(0), NP(1)); return 0; }
uint32_t aret_RtlAnsiStringToUnicodeString(uint32_t esp) { return (uint32_t)RtlAnsiStringToUnicodeString(NP(0), NP(1), (unsigned char)NA(2)); }
uint32_t aret_RtlUnicodeStringToAnsiString(uint32_t esp) { return (uint32_t)RtlUnicodeStringToAnsiString(NP(0), NP(1), (unsigned char)NA(2)); }
uint32_t aret_RtlOemStringToUnicodeString(uint32_t esp) { return (uint32_t)RtlOemStringToUnicodeString(NP(0), NP(1), (unsigned char)NA(2)); }
uint32_t aret_RtlUnicodeStringToOemString(uint32_t esp) { return (uint32_t)RtlUnicodeStringToOemString(NP(0), NP(1), (unsigned char)NA(2)); }
uint32_t aret_RtlFreeAnsiString(uint32_t esp) { RtlFreeAnsiString(NP(0)); return 0; }
uint32_t aret_RtlFreeUnicodeString(uint32_t esp) { RtlFreeUnicodeString(NP(0)); return 0; }
uint32_t aret_RtlFreeOemString(uint32_t esp) { RtlFreeOemString(NP(0)); return 0; }
uint32_t aret_RtlEqualString(uint32_t esp) { return RtlEqualString(NP(0), NP(1), (unsigned char)NA(2)); }
uint32_t aret_RtlEqualUnicodeString(uint32_t esp) { return RtlEqualUnicodeString(NP(0), NP(1), (unsigned char)NA(2)); }
uint32_t aret_RtlCompareString(uint32_t esp) { return (uint32_t)RtlCompareString(NP(0), NP(1), (unsigned char)NA(2)); }
uint32_t aret_RtlCompareUnicodeString(uint32_t esp) { return (uint32_t)RtlCompareUnicodeString(NP(0), NP(1), (unsigned char)NA(2)); }
uint32_t aret_RtlIntegerToChar(uint32_t esp) { return (uint32_t)RtlIntegerToChar(NA(0), NA(1), NA(2), (char *)NP(3)); }
uint32_t aret_RtlIntegerToUnicodeString(uint32_t esp) { return (uint32_t)RtlIntegerToUnicodeString(NA(0), NA(1), NP(2)); }
uint32_t aret_RtlCharToInteger(uint32_t esp) { return (uint32_t)RtlCharToInteger((const char *)NP(0), NA(1), (uint32_t *)NP(2)); }
uint32_t aret_RtlUnicodeStringToInteger(uint32_t esp) { return (uint32_t)RtlUnicodeStringToInteger(NP(0), NA(1), (uint32_t *)NP(2)); }
uint32_t aret_RtlCreateUnicodeString(uint32_t esp) { return RtlCreateUnicodeString(NP(0), NP(1)); }
uint32_t aret_RtlCreateUnicodeStringFromAsciiz(uint32_t esp) { return RtlCreateUnicodeStringFromAsciiz(NP(0), (const char *)NP(1)); }
uint32_t aret_RtlAppendUnicodeStringToString(uint32_t esp) { return (uint32_t)RtlAppendUnicodeStringToString(NP(0), NP(1)); }
uint32_t aret_RtlAppendUnicodeToString(uint32_t esp) { return (uint32_t)RtlAppendUnicodeToString(NP(0), NP(1)); }
uint32_t aret_RtlPrefixString(uint32_t esp) { return RtlPrefixString(NP(0), NP(1), (unsigned char)NA(2)); }
uint32_t aret_RtlpNtCreateKey(uint32_t esp) { return (uint32_t)RtlpNtCreateKey(NP(0), NA(1), NP(2), NA(3), NP(4), NA(5), NP(6)); }
uint32_t aret_RtlpNtOpenKey(uint32_t esp) { return (uint32_t)RtlpNtOpenKey(NP(0), NA(1), NP(2)); }
uint32_t aret_RtlpNtSetValueKey(uint32_t esp) { return (uint32_t)RtlpNtSetValueKey(NP(0), NA(1), NP(2), NA(3)); }
uint32_t aret_RtlpNtQueryValueKey(uint32_t esp) { return (uint32_t)RtlpNtQueryValueKey(NP(0), NP(1), NP(2), NP(3), NP(4)); }

#else /* not native i386: the compiled Wine bodies are not linked -> sound abort, never a guess */
#define STUB(n) uint32_t aret_##n(uint32_t esp) { (void)esp; aret_unimpl(#n); return 0; }
STUB(RtlInitString) STUB(RtlInitAnsiString) STUB(RtlInitUnicodeString)
STUB(RtlAnsiStringToUnicodeString) STUB(RtlUnicodeStringToAnsiString)
STUB(RtlOemStringToUnicodeString) STUB(RtlUnicodeStringToOemString)
STUB(RtlFreeAnsiString) STUB(RtlFreeUnicodeString) STUB(RtlFreeOemString)
STUB(RtlEqualString) STUB(RtlEqualUnicodeString) STUB(RtlCompareString) STUB(RtlCompareUnicodeString)
STUB(RtlIntegerToChar) STUB(RtlIntegerToUnicodeString) STUB(RtlCharToInteger) STUB(RtlUnicodeStringToInteger)
STUB(RtlCreateUnicodeString) STUB(RtlCreateUnicodeStringFromAsciiz)
STUB(RtlAppendUnicodeStringToString) STUB(RtlAppendUnicodeToString)
STUB(RtlPrefixString)
STUB(RtlpNtCreateKey) STUB(RtlpNtOpenKey) STUB(RtlpNtSetValueKey) STUB(RtlpNtQueryValueKey)
STUB(RtlMultiByteToUnicodeN) STUB(RtlUnicodeToMultiByteN) STUB(RtlUpcaseUnicodeToMultiByteN)
STUB(RtlOemToUnicodeN) STUB(RtlUnicodeToOemN) STUB(RtlUpcaseUnicodeToOemN)
STUB(RtlMultiByteToUnicodeSize) STUB(RtlUnicodeToMultiByteSize)
STUB(RtlOemStringToUnicodeSize) STUB(RtlUnicodeStringToOemSize)
#undef STUB
#endif
