/* Cooperative threads (fibers), increment 4 (doc 80): Mutex + Semaphore + per-fiber
 * TLS + _beginthreadex (the msvcrt CRT thread wrapper).
 *  - Mutex: workers increment a shared counter under WaitForSingleObject(mtx)/
 *    ReleaseMutex, with the read-modify-write split by a Sleep(0) inside the lock —
 *    a genuine discriminator (no exclusion => lost updates). => mcounter = NT*500.
 *  - Per-fiber TLS: each worker stores its id, yields (another worker stores ITS
 *    id), then reads back — must still see its own (global TLS would be clobbered).
 *  - Semaphore: producer ReleaseSemaphore x SK, consumer WaitForSingleObject x SK.
 * Deterministic on Wine (preemptive) and ARET (cooperative). Console fixture. */
#include <windows.h>
#include <process.h>
#include <stdio.h>

#define NT 4
static HANDLE mtx;
static long mcounter = 0;
static DWORD tlsSlot;
static int tls_ok[NT];

static unsigned __stdcall worker(void *p) {
    int id = (int)(INT_PTR)p;
    TlsSetValue(tlsSlot, (LPVOID)(INT_PTR)(id + 100));
    Sleep(0);                                    /* yield: another worker sets ITS tls */
    tls_ok[id] = ((int)(INT_PTR)TlsGetValue(tlsSlot) == id + 100) ? 1 : 0;
    for (int i = 0; i < 500; i++) {
        WaitForSingleObject(mtx, INFINITE);
        long v = mcounter;
        Sleep(0);                                /* split the RMW under the mutex */
        mcounter = v + 1;
        ReleaseMutex(mtx);
    }
    return 0;
}

#define SK 6
static HANDLE sem;
static int sitems[SK];
static long ssum = 0;

static unsigned __stdcall producer(void *p) {
    (void)p;
    for (int i = 0; i < SK; i++) { sitems[i] = i + 1; ReleaseSemaphore(sem, 1, NULL); }
    return 0;
}

int main(void) {
    tlsSlot = TlsAlloc();
    mtx = CreateMutexA(NULL, FALSE, NULL);
    HANDLE h[NT];
    for (int i = 0; i < NT; i++)
        h[i] = (HANDLE)_beginthreadex(NULL, 0, worker, (void *)(INT_PTR)i, 0, NULL);
    WaitForMultipleObjects(NT, h, TRUE, INFINITE);
    for (int i = 0; i < NT; i++) CloseHandle(h[i]);
    int allok = 1; for (int i = 0; i < NT; i++) if (!tls_ok[i]) allok = 0;
    printf("mcounter=%ld (expect %d)\n", mcounter, NT * 500);
    printf("tls_ok=%d\n", allok);

    sem = CreateSemaphoreA(NULL, 0, SK, NULL);      /* count 0, max SK */
    HANDLE hp = (HANDLE)_beginthreadex(NULL, 0, producer, NULL, 0, NULL);
    for (int i = 0; i < SK; i++) { WaitForSingleObject(sem, INFINITE); ssum += sitems[i]; }
    WaitForSingleObject(hp, INFINITE);
    CloseHandle(hp); CloseHandle(sem); CloseHandle(mtx); TlsFree(tlsSlot);
    printf("ssum=%ld (expect %d)\n", ssum, SK * (SK + 1) / 2);
    printf("done\n");
    return 0;
}
