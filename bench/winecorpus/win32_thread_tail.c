/* Thread long-tail family (measured head of the Win95 plateau, display-free):
 *  - SetThreadPriority/GetThreadPriority: a scheduling hint. ARET's cooperative
 *    scheduler is deterministic round-robin, so priority only round-trips (store
 *    then read back), it does not reorder — the observable API contract Wine gives.
 *  - OpenProcess: own pid succeeds, a bogus pid fails with ERROR_INVALID_PARAMETER.
 *  - TerminateThread: forcibly ends a parked worker; it never resumes, its exit code
 *    is the TerminateThread argument, and it becomes signaled.
 * All values measured against Wine and deterministic (no display / no wall-clock). */
#include <windows.h>
#include <stdio.h>

static volatile int g_ran = 0;
static DWORD WINAPI worker(LPVOID p) {
    (void)p;
    g_ran = 1;
    Sleep(100000);            /* park; the test terminates it before this returns */
    g_ran = 2;                /* must never be reached */
    return 77;
}

int main(void) {
    HANDLE self = GetCurrentThread();
    int p0 = GetThreadPriority(self);
    BOOL sp = SetThreadPriority(self, THREAD_PRIORITY_ABOVE_NORMAL);
    int p1 = GetThreadPriority(self);
    printf("prio default=%d set=%d after=%d\n", p0, sp, p1);

    HANDLE po = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    HANDLE pb = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, 0x0BADF00D);
    printf("openproc self=%d bogus=%d bogus_err=%lu\n",
           po != NULL, pb != NULL, pb ? 0UL : (unsigned long)GetLastError());

    DWORD tid;
    HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, &tid);
    Sleep(10);                /* let the worker reach g_ran=1 then park */
    BOOL tr = TerminateThread(h, 55);
    Sleep(10);                /* the worker must not resume to g_ran=2 */
    DWORD ec = 0; GetExitCodeThread(h, &ec);
    DWORD w = WaitForSingleObject(h, 1000);
    printf("terminate ret=%d ran=%d exit=%lu wait=%lu\n",
           tr, g_ran, (unsigned long)ec, (unsigned long)w);
    CloseHandle(h);
    printf("done\n");
    return 0;
}
