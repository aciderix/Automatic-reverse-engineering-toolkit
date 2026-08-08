/* Heavy-form real-ABI Nt* REGISTRY floor (doc 82 tranche 5). A COMPILED Wine ntdll .c that calls
 * the NT registry syscalls links these real NTAPI wrappers; each one just casts its real pointers
 * to 32-bit addresses and calls the shared aret_ntreg_* cores (ARET's g_reg, aret_win32.c). Whole
 * thing is one 32-bit address space, so (uint32_t)(uintptr_t)ptr is exact -- the same identity the
 * esp shims rely on. Kept SEPARATE from ntdll_floor.c so the string-floor proofs (which do not call
 * the registry) link unchanged; production compiles both and resolves aret_ntreg_* from aret_win32.c.
 * Proven standalone bit-identical Wine by tools/wine_heavy/proof_ntreg.sh. */
#include <stdint.h>
#define NTAPI __attribute__((stdcall))
typedef long NTSTATUS; typedef uint16_t WCHAR; typedef unsigned long ULONG;
typedef void *HANDLE; typedef unsigned long ACCESS_MASK;
typedef struct { unsigned short Length, MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
typedef struct { ULONG Length; HANDLE RootDirectory; UNICODE_STRING *ObjectName;
                 ULONG Attributes; void *SecurityDescriptor, *SecurityQualityOfService; } OBJECT_ATTRIBUTES;

/* the shared cores (ARET's g_reg; the reference proof provides equivalents) */
extern uint32_t aret_ntreg_create(uint32_t poa, uint32_t phkey, uint32_t pdisp);
extern uint32_t aret_ntreg_open(uint32_t poa, uint32_t phkey);
extern uint32_t aret_ntreg_setval(uint32_t hkey, uint32_t pvalname, uint32_t type, uint32_t data, uint32_t size);
extern uint32_t aret_ntreg_queryval(uint32_t hkey, uint32_t pvalname, uint32_t cls, uint32_t info, uint32_t length, uint32_t presult);
extern uint32_t aret_ntreg_delval(uint32_t hkey, uint32_t pvalname);
extern uint32_t aret_ntreg_enumkey(uint32_t hkey, uint32_t index, uint32_t cls, uint32_t info, uint32_t length, uint32_t presult);
extern uint32_t aret_ntreg_enumval(uint32_t hkey, uint32_t index, uint32_t cls, uint32_t info, uint32_t length, uint32_t presult);
extern uint32_t aret_ntreg_delkey(uint32_t hkey);
extern int aret_ntfile_close(uint32_t handle);
extern void aret_unimpl(const char *);

#define AA(p) ((uint32_t)(uintptr_t)(p))

NTSTATUS NTAPI NtCreateKey(HANDLE *KeyHandle, ACCESS_MASK acc, OBJECT_ATTRIBUTES *oa,
                           ULONG TitleIndex, UNICODE_STRING *Class, ULONG Options, ULONG *Disposition) {
    (void)acc; (void)TitleIndex; (void)Class; (void)Options;
    return aret_ntreg_create(AA(oa), AA(KeyHandle), AA(Disposition));
}
NTSTATUS NTAPI NtOpenKey(HANDLE *KeyHandle, ACCESS_MASK acc, OBJECT_ATTRIBUTES *oa) {
    (void)acc; return aret_ntreg_open(AA(oa), AA(KeyHandle));
}
NTSTATUS NTAPI NtSetValueKey(HANDLE k, UNICODE_STRING *ValueName, ULONG TitleIndex,
                             ULONG Type, void *Data, ULONG DataSize) {
    (void)TitleIndex; return aret_ntreg_setval(AA(k), AA(ValueName), Type, AA(Data), DataSize);
}
NTSTATUS NTAPI NtQueryValueKey(HANDLE k, UNICODE_STRING *ValueName, int KeyValueInformationClass,
                               void *KeyValueInformation, ULONG Length, ULONG *ResultLength) {
    return aret_ntreg_queryval(AA(k), AA(ValueName), (uint32_t)KeyValueInformationClass,
                               AA(KeyValueInformation), Length, AA(ResultLength));
}
NTSTATUS NTAPI NtDeleteValueKey(HANDLE k, UNICODE_STRING *ValueName) {
    return aret_ntreg_delval(AA(k), AA(ValueName));
}
NTSTATUS NTAPI NtEnumerateKey(HANDLE k, ULONG Index, int KeyInformationClass,
                              void *KeyInformation, ULONG Length, ULONG *ResultLength) {
    return aret_ntreg_enumkey(AA(k), Index, (uint32_t)KeyInformationClass, AA(KeyInformation), Length, AA(ResultLength));
}
NTSTATUS NTAPI NtEnumerateValueKey(HANDLE k, ULONG Index, int KeyValueInformationClass,
                                   void *KeyValueInformation, ULONG Length, ULONG *ResultLength) {
    return aret_ntreg_enumval(AA(k), Index, (uint32_t)KeyValueInformationClass, AA(KeyValueInformation), Length, AA(ResultLength));
}
NTSTATUS NTAPI NtDeleteKey(HANDLE k) { return aret_ntreg_delkey(AA(k)); }
NTSTATUS NTAPI NtClose(HANDLE h) { aret_ntfile_close(AA(h)); return 0; }
/* NtQueryInformationToken: token/SID API, reached only by RtlOpenCurrentUser (off the modelled
 * registry round-trip path). Not modelled -> sound abort if ever actually called; provides the
 * symbol so a whole compiled reg.c links. */
NTSTATUS NTAPI NtQueryInformationToken(HANDLE Token, int Class, void *Info, ULONG Len, ULONG *RetLen) {
    (void)Token; (void)Class; (void)Info; (void)Len; (void)RetLen;
    aret_unimpl("NtQueryInformationToken (RtlOpenCurrentUser)"); return -1;
}
