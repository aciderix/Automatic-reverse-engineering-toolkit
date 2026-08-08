/* CAPSTONE end-to-end (doc 82 tranche 6): a real PE imports ntdll's RtlpNtCreateKey/RtlpNtSetValueKey/
 * RtlpNtQueryValueKey -- exported thin wrappers whose bodies live in Wine's dlls/ntdll/reg.c. Under
 * ARET these imports route (aret_ntdll.c adapters) to the WHOLE reg.c COMPILED into the binary, which
 * calls the real-ABI Nt* registry floor -> g_reg; under Wine, to the real ntdll. Both round-trip a
 * caller-provided key, so it is bit-identical Wine -- a PE reaching compiled Wine logic on ARET's floor. */
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
NTSTATUS WINAPI RtlpNtCreateKey(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,ULONG,PUNICODE_STRING,ULONG,PULONG);
NTSTATUS WINAPI RtlpNtOpenKey(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES);
NTSTATUS WINAPI RtlpNtSetValueKey(HANDLE,ULONG,const void*,ULONG);
NTSTATUS WINAPI RtlpNtQueryValueKey(HANDLE,ULONG*,void*,ULONG*,void*);
NTSTATUS WINAPI RtlpNtEnumerateSubKey(HANDLE,PUNICODE_STRING,ULONG);
NTSTATUS WINAPI NtClose(HANDLE);
int main(void){
    UNICODE_STRING kp; RtlInitUnicodeString(&kp,L"\\Registry\\Machine\\Software\\AretRtlpProof");
    OBJECT_ATTRIBUTES oa; InitializeObjectAttributes(&oa,&kp,OBJ_CASE_INSENSITIVE,NULL,NULL);
    HANDLE h=NULL; ULONG disp=0;
    NTSTATUS s=RtlpNtCreateKey(&h,KEY_ALL_ACCESS,&oa,0,NULL,0,&disp);
    printf("create hr=0x%08lX disp=%lu\n",(unsigned long)s,(unsigned long)disp);
    DWORD data=42;
    s=RtlpNtSetValueKey(h,REG_DWORD,&data,sizeof data);
    printf("set hr=0x%08lX\n",(unsigned long)s);
    unsigned char buf[64]; ULONG type=0, count=sizeof buf;
    s=RtlpNtQueryValueKey(h,&type,buf,&count,NULL);
    printf("query hr=0x%08lX type=%lu count=%lu val=%lu\n",(unsigned long)s,(unsigned long)type,(unsigned long)count,(unsigned long)*(DWORD*)buf);

    /* create two subkeys under the root (relative, via RootDirectory) -- exercises the compiled
     * reg.c RtlpNtCreateKey/relative path + the floor's NtCreateKey with RootDirectory */
    HANDLE sk; UNICODE_STRING zn,an; OBJECT_ATTRIBUTES za,aa;
    RtlInitUnicodeString(&zn,L"Zenith"); InitializeObjectAttributes(&za,&zn,OBJ_CASE_INSENSITIVE,h,NULL);
    RtlpNtCreateKey(&sk,KEY_ALL_ACCESS,&za,0,NULL,0,NULL); NtClose(sk);
    RtlInitUnicodeString(&an,L"Apex");   InitializeObjectAttributes(&aa,&an,OBJ_CASE_INSENSITIVE,h,NULL);
    RtlpNtCreateKey(&sk,KEY_ALL_ACCESS,&aa,0,NULL,0,NULL); NtClose(sk);

    /* enumerate subkeys (compiled reg.c RtlpNtEnumerateSubKey -> floor NtEnumerateKey), sorted */
    for (ULONG i=0;;i++){
        WCHAR nb[64]; UNICODE_STRING sub; sub.Buffer=nb; sub.MaximumLength=sizeof nb; sub.Length=0;
        s=RtlpNtEnumerateSubKey(h,&sub,i);
        if (s){ printf("enum[%lu] hr=0x%08lX\n",(unsigned long)i,(unsigned long)s); break; }
        printf("enum[%lu] len=%u name=",(unsigned long)i,sub.Length);
        for (int j=0;j<sub.Length/2;j++) putchar((char)sub.Buffer[j]); putchar('\n');
        if (i>10) break;
    }

    /* reopen the root by name (compiled reg.c RtlpNtOpenKey -> floor NtOpenKey) and re-read */
    HANDLE h2=NULL; s=RtlpNtOpenKey(&h2,KEY_READ,&oa);
    ULONG t2=0,c2=sizeof buf; RtlpNtQueryValueKey(h2,&t2,buf,&c2,NULL);
    printf("reopen hr=0x%08lX val=%lu\n",(unsigned long)s,(unsigned long)*(DWORD*)buf);
    NtClose(h2); NtClose(h);
    return 0;
}
