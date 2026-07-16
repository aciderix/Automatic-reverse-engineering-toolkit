/* Cooperative threads (fibers), increment 3 (doc 80): real Event objects.
 *  Part 1 — MANUAL-reset "gate": workers block on it, a releaser thread SetEvents
 *    it once, ALL workers are released (a manual event stays signaled). If it only
 *    released one, the join would deadlock -> sound abort. Sum is order-independent.
 *  Part 2 — AUTO-reset ping-pong: producer/consumer hand off via two auto-reset
 *    events; each SetEvent releases exactly one waiter then self-resets, giving a
 *    strict deterministic sequence. Deterministic on Wine (preemptive) and ARET
 *    (cooperative) alike. Console fixture. */
#include <windows.h>
#include <stdio.h>

#define NT 3
static HANDLE gate;
static CRITICAL_SECTION cs;
static long gate_sum = 0;

static DWORD WINAPI gated(LPVOID p) {
    int id = (int)(INT_PTR)p;
    WaitForSingleObject(gate, INFINITE);       /* block until the gate opens */
    EnterCriticalSection(&cs);
    gate_sum += (id + 1) * 10;                  /* 10+20+30 = 60, order-independent */
    LeaveCriticalSection(&cs);
    return 0;
}
static DWORD WINAPI releaser(LPVOID p) { (void)p; SetEvent(gate); return 0; }

#define K 5
static HANDLE eReady, eDone;
static int item;
static long pp_sum = 0;

static DWORD WINAPI producer(LPVOID p) {
    (void)p;
    for (int i = 1; i <= K; i++) {
        item = i;
        SetEvent(eReady);
        WaitForSingleObject(eDone, INFINITE);
    }
    return 0;
}

int main(void) {
    InitializeCriticalSection(&cs);
    /* Part 1: manual-reset gate. Workers first (so they block), then the releaser. */
    gate = CreateEventA(NULL, TRUE, FALSE, NULL);   /* manual-reset, non-signaled */
    HANDLE h[NT + 1];
    for (int i = 0; i < NT; i++) h[i] = CreateThread(NULL, 0, gated, (LPVOID)(INT_PTR)i, 0, NULL);
    h[NT] = CreateThread(NULL, 0, releaser, NULL, 0, NULL);
    WaitForMultipleObjects(NT + 1, h, TRUE, INFINITE);
    for (int i = 0; i < NT + 1; i++) CloseHandle(h[i]);
    printf("gate_sum=%ld (expect 60)\n", gate_sum);

    /* Part 2: auto-reset ping-pong. */
    eReady = CreateEventA(NULL, FALSE, FALSE, NULL);  /* auto-reset */
    eDone  = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE hp = CreateThread(NULL, 0, producer, NULL, 0, NULL);
    for (int i = 0; i < K; i++) {
        WaitForSingleObject(eReady, INFINITE);
        pp_sum += item;
        SetEvent(eDone);
    }
    WaitForSingleObject(hp, INFINITE);
    CloseHandle(hp); CloseHandle(eReady); CloseHandle(eDone); CloseHandle(gate);
    DeleteCriticalSection(&cs);
    printf("pp_sum=%ld (expect %d)\n", pp_sum, K * (K + 1) / 2);
    printf("done\n");
    return 0;
}
