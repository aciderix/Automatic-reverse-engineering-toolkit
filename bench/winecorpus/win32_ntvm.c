/* ntdll Nt* tranche 4 (doc 82): NtAllocateVirtualMemory / NtFreeVirtualMemory -- the syscalls
 * VirtualAlloc / RtlAllocateHeap bottom out on. The returned base address is non-deterministic, so
 * it is NOT compared -- the fixture proves the deterministic contract: STATUS_SUCCESS, RegionSize
 * page-rounded (measured 100->4096, 5000->8192), memory zeroed and writable, free succeeds. */
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
typedef LONG NTSTATUS;
NTSTATUS WINAPI NtAllocateVirtualMemory(HANDLE,PVOID*,ULONG_PTR,PSIZE_T,ULONG,ULONG);
NTSTATUS WINAPI NtFreeVirtualMemory(HANDLE,PVOID*,PSIZE_T,ULONG);
#ifndef MEM_COMMIT
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_READWRITE 0x04
#endif
int main(void){
    PVOID base=NULL; SIZE_T sz=100;
    NTSTATUS s=NtAllocateVirtualMemory((HANDLE)-1,&base,0,&sz,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    printf("alloc s=0x%08lX base_nonnull=%d RegionSize=%lu\n",(unsigned long)s,base!=NULL,(unsigned long)sz);
    unsigned char* p=base;
    int zeroed = (p[0]==0 && p[50]==0 && p[99]==0);
    p[0]=0xAB; p[99]=0xCD;
    printf("zeroed=%d readback=%02X,%02X\n",zeroed,p[0],p[99]);
    PVOID fb=base; SIZE_T fs=0;
    s=NtFreeVirtualMemory((HANDLE)-1,&fb,&fs,MEM_RELEASE);
    printf("free s=0x%08lX\n",(unsigned long)s);

    base=NULL; sz=5000;
    s=NtAllocateVirtualMemory((HANDLE)-1,&base,0,&sz,MEM_COMMIT,PAGE_READWRITE);
    printf("alloc2 s=0x%08lX RegionSize=%lu\n",(unsigned long)s,(unsigned long)sz);
    fb=base; fs=0; s=NtFreeVirtualMemory((HANDLE)-1,&fb,&fs,MEM_RELEASE);
    printf("free2 s=0x%08lX\n",(unsigned long)s);
    return 0;
}
