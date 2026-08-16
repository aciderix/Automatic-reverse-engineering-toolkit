/* SRWLOCK + CONDITION_VARIABLE (modern Win32 threading, the first family enabling the
 * libglib lift — doc 82). Runs on ARET's cooperative fiber scheduler. Deterministic:
 * (1) four threads each do 1000 increments of a shared counter under an EXCLUSIVE SRW
 * lock with a yield (Sleep(0)) cutting the read-modify-write — only true mutual
 * exclusion yields 4000 (a no-op lock would lose increments); (2) a condition variable
 * ping: main waits under the lock until a signaler thread sets a flag and wakes it. */
#include <windows.h>
#include <stdio.h>

static SRWLOCK g_lock;
static CONDITION_VARIABLE g_cv;
static long g_counter = 0;
static int g_ready = 0;

static DWORD WINAPI worker(LPVOID p) {
    (void)p;
    for (int i = 0; i < 1000; i++) {
        AcquireSRWLockExclusive(&g_lock);
        long v = g_counter;
        Sleep(0);                       /* yield under the lock: forces the RMW to serialize */
        g_counter = v + 1;
        ReleaseSRWLockExclusive(&g_lock);
    }
    return 0;
}
static DWORD WINAPI signaler(LPVOID p) {
    (void)p;
    AcquireSRWLockExclusive(&g_lock);
    g_ready = 1;
    ReleaseSRWLockExclusive(&g_lock);
    WakeConditionVariable(&g_cv);
    return 0;
}

int main(void) {
    InitializeSRWLock(&g_lock);
    InitializeConditionVariable(&g_cv);

    HANDLE t[4];
    for (int i = 0; i < 4; i++) t[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    WaitForMultipleObjects(4, t, TRUE, INFINITE);
    for (int i = 0; i < 4; i++) CloseHandle(t[i]);
    printf("counter=%ld\n", g_counter);           /* 4000 iff the lock serializes the RMW */

    HANDLE s = CreateThread(NULL, 0, signaler, NULL, 0, NULL);
    AcquireSRWLockExclusive(&g_lock);
    while (!g_ready) SleepConditionVariableSRW(&g_cv, &g_lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&g_lock);
    WaitForSingleObject(s, INFINITE);
    CloseHandle(s);
    printf("cv_ready=%d\n", g_ready);              /* 1 */

    /* shared read lock: two readers hold it at once, TryAcquireExclusive must fail. */
    AcquireSRWLockShared(&g_lock);
    AcquireSRWLockShared(&g_lock);
    printf("try_excl_while_shared=%d\n", TryAcquireSRWLockExclusive(&g_lock));  /* 0 */
    ReleaseSRWLockShared(&g_lock);
    ReleaseSRWLockShared(&g_lock);
    printf("try_excl_after=%d\n", TryAcquireSRWLockExclusive(&g_lock));         /* 1 */
    ReleaseSRWLockExclusive(&g_lock);

    printf("done\n");
    return 0;
}
