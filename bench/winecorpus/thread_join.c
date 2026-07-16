/* Cooperative threads (fibers), increment 1 (doc 80): CreateThread + join via
 * WaitForMultipleObjects + per-thread GetLastError + GetExitCodeThread. The oracle
 * is deterministic on both Wine (real preemptive threads) and ARET (cooperative
 * fibers) because each worker writes to its OWN slot (no shared read-modify-write),
 * and the totals/exit codes are order-independent. Console fixture (no display). */
#include <windows.h>
#include <stdio.h>

#define NT 4
static int results[NT];
static int lasterr_ok[NT];

static DWORD WINAPI worker(LPVOID p) {
    int id = (int)(INT_PTR)p;
    SetLastError((DWORD)(0xB000 + id));
    Sleep(0);                                  /* yield: another fiber runs in between */
    lasterr_ok[id] = (GetLastError() == (DWORD)(0xB000 + id)) ? 1 : 0;   /* per-thread */
    int s = 0;
    for (int i = 0; i < 100; i++) s += i;      /* 4950 */
    results[id] = id * 1000 + s;
    return (DWORD)(id + 1);                     /* exit code */
}

int main(void) {
    HANDLE h[NT];
    DWORD tid[NT];
    for (int i = 0; i < NT; i++)
        h[i] = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)i, 0, &tid[i]);
    DWORD w = WaitForMultipleObjects(NT, h, TRUE, INFINITE);
    printf("wait=%lu\n", (unsigned long)w);
    long total = 0; int allok = 1;
    for (int i = 0; i < NT; i++) {
        total += results[i];
        if (!lasterr_ok[i]) allok = 0;
        DWORD ec = 0; GetExitCodeThread(h[i], &ec);
        printf("thread %d exit=%lu\n", i, (unsigned long)ec);
        CloseHandle(h[i]);
    }
    printf("total=%ld lasterr_ok=%d\n", total, allok);
    printf("done\n");
    return 0;
}
