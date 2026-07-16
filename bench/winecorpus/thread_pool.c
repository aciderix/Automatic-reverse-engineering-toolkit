/* Realistic composed thread workload (doc 80 fibers, incr. 5 = finite timeouts).
 * A thread pool drains a shared work queue, exercising ALL the sync primitives at
 * once the way real code does — crucially with a FINITE timeout used as a liveness
 * poll (WaitForSingleObject(sem, 50)), which a naive "finite==infinite" model would
 * deadlock on. A deterministic virtual clock honours the timeout instead.
 *   - MUTEX guards the queue; SEMAPHORE counts queued items; manual-reset EVENT is
 *     the stop flag; each worker keeps its partial sum in PER-FIBER TLS.
 * Parallel sum of i*i for i in 1..M — verifiable, order-independent. Console. */
#include <windows.h>
#include <process.h>
#include <stdio.h>

#define NW 4
#define M  200
static HANDLE qMutex, qSem, doneEvt;
static int queue[M], qHead = 0, qCount = 0;
static DWORD tls;
static long long partials[NW];
static HANDLE hw[NW];

static unsigned __stdcall worker(void *p) {
    int id = (int)(INT_PTR)p;
    TlsSetValue(tls, (LPVOID)(INT_PTR)id);
    long long acc = 0;
    for (;;) {
        DWORD r = WaitForSingleObject(qSem, 50);          /* finite timeout = liveness poll */
        if (r != WAIT_OBJECT_0) {
            if (WaitForSingleObject(doneEvt, 0) == WAIT_OBJECT_0 && qCount == 0) break;
            continue;
        }
        WaitForSingleObject(qMutex, INFINITE);
        int v = (qCount > 0) ? queue[qHead++] : 0, got = (qCount > 0);
        if (qCount > 0) qCount--;
        ReleaseMutex(qMutex);
        if (got) acc += (long long)v * v;
    }
    partials[(int)(INT_PTR)TlsGetValue(tls)] = acc;       /* still my own id */
    return 0;
}

int main(void) {
    tls = TlsAlloc();
    qMutex = CreateMutexA(NULL, FALSE, NULL);
    qSem = CreateSemaphoreA(NULL, 0, M, NULL);
    doneEvt = CreateEventA(NULL, TRUE, FALSE, NULL);
    for (int i = 0; i < NW; i++) hw[i] = (HANDLE)_beginthreadex(NULL, 0, worker, (void *)(INT_PTR)i, 0, NULL);
    for (int i = 1; i <= M; i++) {
        WaitForSingleObject(qMutex, INFINITE);
        queue[qHead + qCount] = i; qCount++;
        ReleaseMutex(qMutex);
        ReleaseSemaphore(qSem, 1, NULL);
    }
    SetEvent(doneEvt);
    WaitForMultipleObjects(NW, hw, TRUE, INFINITE);
    long long total = 0; for (int i = 0; i < NW; i++) { total += partials[i]; CloseHandle(hw[i]); }
    long long expect = 0; for (int i = 1; i <= M; i++) expect += (long long)i * i;
    printf("total=%lld expect=%lld %s\n", total, expect, total == expect ? "OK" : "MISMATCH");
    printf("done\n");
    return 0;
}
