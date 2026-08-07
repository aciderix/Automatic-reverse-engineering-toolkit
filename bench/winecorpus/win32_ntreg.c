/* ntdll Nt* registry floor (doc 82): NtCreateKey/NtSetValueKey/NtQueryValueKey/NtDeleteValueKey/
 * NtClose on ARET's in-memory registry (same g_reg as advapi32 Reg*). Empty by design, so this
 * ROUND-TRIPS (create -> set -> query) -- bit-identical Wine, which round-trips its own registry. */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
typedef struct { ULONG TitleIndex, Type, DataLength; UCHAR Data[1]; } KVPI;
NTSTATUS WINAPI NtCreateKey(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,ULONG,PUNICODE_STRING,ULONG,PULONG);
NTSTATUS WINAPI NtOpenKey(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES);
NTSTATUS WINAPI NtSetValueKey(HANDLE,PUNICODE_STRING,ULONG,ULONG,PVOID,ULONG);
NTSTATUS WINAPI NtQueryValueKey(HANDLE,PUNICODE_STRING,int,PVOID,ULONG,PULONG);
NTSTATUS WINAPI NtDeleteValueKey(HANDLE,PUNICODE_STRING);
NTSTATUS WINAPI NtClose(HANDLE);

int main(void){
    UNICODE_STRING kp; RtlInitUnicodeString(&kp, L"\\Registry\\Machine\\Software\\AretTest");
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

    /* re-open the same key by name and read again (round-trip through the tree) */
    HANDLE h2=NULL; s=NtOpenKey(&h2,KEY_READ,&oa);
    ULONG rl2=0; NtQueryValueKey(h2,&vn,2,buf,sizeof buf,&rl2);
    printf("reopen hr=0x%08lX val=%lu\n",(unsigned long)s,(unsigned long)*(DWORD*)((KVPI*)buf)->Data);

    /* too-small buffer -> BUFFER_OVERFLOW, ResultLength set */
    ULONG rl3=0; s=NtQueryValueKey(h,&vn,2,buf,13,&rl3);
    printf("small hr=0x%08lX rl=%lu\n",(unsigned long)s,(unsigned long)rl3);

    s=NtDeleteValueKey(h,&vn); printf("del hr=0x%08lX\n",(unsigned long)s);
    ULONG rl4=0; s=NtQueryValueKey(h,&vn,2,buf,sizeof buf,&rl4);
    printf("query-after-del hr=0x%08lX\n",(unsigned long)s);
    NtClose(h); NtClose(h2);
    return 0;
}
