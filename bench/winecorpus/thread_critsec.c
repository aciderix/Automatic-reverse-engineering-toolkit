/* Cooperative threads (fibers), increment 2 (doc 80): a real CRITICAL_SECTION.
 * The canonical counter=4000 oracle, made a genuine discriminator by splitting the
 * read-modify-write with a Sleep(0) yield *inside* the lock: only a correct mutual
 * exclusion (the yielding owner keeps other fibers out until it Leaves) yields the
 * full 4000; a no-op lock loses updates. Deterministic on Wine (real preemptive
 * threads) and ARET (cooperative fibers) alike. Also checks recursion. Console. */
#include <windows.h>
#include <stdio.h>

#define NT 4
#define ITERS 1000
static CRITICAL_SECTION cs;
static long counter = 0;
static int rec_ok = 1;

static DWORD WINAPI worker(LPVOID p) {
    (void)p;
    for (int i = 0; i < ITERS; i++) {
        EnterCriticalSection(&cs);
        long v = counter;
        Sleep(0);                 /* yield while holding the lock: splits the RMW */
        counter = v + 1;
        /* recursion: re-enter, still ours, leave once; count must stay consistent */
        EnterCriticalSection(&cs);
        if (!TryEnterCriticalSection(&cs)) rec_ok = 0;   /* owner can always re-enter */
        LeaveCriticalSection(&cs);
        LeaveCriticalSection(&cs);
        LeaveCriticalSection(&cs);
    }
    return 0;
}

int main(void) {
    InitializeCriticalSection(&cs);
    HANDLE h[NT];
    for (int i = 0; i < NT; i++)
        h[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    WaitForMultipleObjects(NT, h, TRUE, INFINITE);
    for (int i = 0; i < NT; i++) CloseHandle(h[i]);
    DeleteCriticalSection(&cs);
    printf("counter=%ld (expect %d)\n", counter, NT * ITERS);
    printf("rec_ok=%d\n", rec_ok);
    printf("done\n");
    return 0;
}
