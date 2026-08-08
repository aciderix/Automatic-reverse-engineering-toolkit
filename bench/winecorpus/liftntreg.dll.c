/* Companion DLL for liftntreg.c (Levier 1, doc 80 §1.2 + doc 82): a real PE DLL whose exported
 * function does an ntdll Nt* REGISTRY round-trip. ARET LIFTS this binary DLL (--with-dll); the
 * lifted DLL's Nt* imports route through the multi-module loader to ARET's aret_Nt* shims -> the
 * SAME g_reg as everything else. Wine loads the real DLL -> real ntdll. Both round-trip a
 * caller-provided key, so it is bit-identical: a LIFTED binary DLL reaching the Nt* registry
 * floor end-to-end. */
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
typedef LONG NTSTATUS;
NTSTATUS WINAPI NtCreateKey(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,ULONG,PUNICODE_STRING,ULONG,PULONG);
NTSTATUS WINAPI NtSetValueKey(HANDLE,PUNICODE_STRING,ULONG,ULONG,PVOID,ULONG);
NTSTATUS WINAPI NtQueryValueKey(HANDLE,PUNICODE_STRING,int,PVOID,ULONG,PULONG);
NTSTATUS WINAPI NtClose(HANDLE);
typedef struct { ULONG TitleIndex,Type,DataLength; UCHAR Data[1]; } KVPI;

/* create HKLM\Software\AretLiftDll, set value "Lifted"=v (REG_DWORD), read it back, return it. */
__declspec(dllexport) unsigned dll_ntreg_roundtrip(unsigned v){
    UNICODE_STRING kp; RtlInitUnicodeString(&kp,L"\\Registry\\Machine\\Software\\AretLiftDll");
    OBJECT_ATTRIBUTES oa; InitializeObjectAttributes(&oa,&kp,OBJ_CASE_INSENSITIVE,NULL,NULL);
    HANDLE h=NULL; ULONG disp=0;
    if (NtCreateKey(&h,KEY_ALL_ACCESS,&oa,0,NULL,0,&disp)) return 0xFFFFFFFFu;
    UNICODE_STRING vn; RtlInitUnicodeString(&vn,L"Lifted");
    NtSetValueKey(h,&vn,0,REG_DWORD,&v,sizeof v);
    unsigned char buf[64]; ULONG rl=0;
    NtQueryValueKey(h,&vn,2,buf,sizeof buf,&rl);
    NtClose(h);
    return *(unsigned*)((KVPI*)buf)->Data;
}
