/* ntdll Nt* tranche 4 (doc 82): NtDelayExecution -- the syscall Sleep bottoms out on. A negative
 * relative interval (100 ns units) is a delay; it returns STATUS_SUCCESS and the program continues
 * (no APC modelled). The timing itself is not compared -- only the status and that execution
 * proceeds -- so it is deterministic and bit-identical Wine (the alternative was an aret_unimpl
 * abort). */
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
typedef LONG NTSTATUS;
NTSTATUS WINAPI NtDelayExecution(BOOLEAN,PLARGE_INTEGER);
int main(void){
    LARGE_INTEGER d;
    d.QuadPart=-100000;   /* 10 ms relative (100000 * 100 ns) */
    printf("delay(-10ms) s=0x%08lX\n",(unsigned long)NtDelayExecution(FALSE,&d));
    d.QuadPart=-50000;    /* 5 ms */
    printf("delay(-5ms) s=0x%08lX\n",(unsigned long)NtDelayExecution(FALSE,&d));
    d.QuadPart=0;         /* yield */
    printf("delay(0) s=0x%08lX\n",(unsigned long)NtDelayExecution(FALSE,&d));
    printf("done\n");
    return 0;
}
