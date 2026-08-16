/* Vista thread-pool work API + alertable waits (libglib residual, doc 82), on ARET's
 * fiber scheduler. Deterministic: N submits of a work item each add its context value
 * to a shared sum under a CRITICAL_SECTION; WaitForThreadpoolWorkCallbacks runs them
 * all to completion. A second work is submitted then cancelled -> its callback must NOT
 * run. WaitForSingleObjectEx on a signaled event returns immediately. */
#include <windows.h>
#include <stdio.h>

static CRITICAL_SECTION g_cs;
static long g_sum = 0;
static int g_ran = 0, g_cancel_ran = 0;

static VOID CALLBACK work_cb(PTP_CALLBACK_INSTANCE inst, PVOID ctx, PTP_WORK work) {
    (void)inst; (void)work;
    EnterCriticalSection(&g_cs);
    g_sum += (long)(LONG_PTR)ctx;
    g_ran++;
    LeaveCriticalSection(&g_cs);
}
static VOID CALLBACK cancel_cb(PTP_CALLBACK_INSTANCE inst, PVOID ctx, PTP_WORK work) {
    (void)inst; (void)ctx; (void)work;
    g_cancel_ran = 1;                         /* must never run (cancelled) */
}

int main(void) {
    InitializeCriticalSection(&g_cs);

    PTP_WORK w = CreateThreadpoolWork(work_cb, (PVOID)(LONG_PTR)7, NULL);
    for (int i = 0; i < 5; i++) SubmitThreadpoolWork(w);      /* 5 x 7 = 35 */
    WaitForThreadpoolWorkCallbacks(w, FALSE);
    CloseThreadpoolWork(w);
    printf("sum=%ld ran=%d\n", g_sum, g_ran);                /* 35 5 */

    PTP_WORK c = CreateThreadpoolWork(cancel_cb, NULL, NULL);
    SubmitThreadpoolWork(c);
    WaitForThreadpoolWorkCallbacks(c, TRUE);                 /* cancel before it runs */
    CloseThreadpoolWork(c);
    printf("cancel_ran=%d\n", g_cancel_ran);                 /* 0 */

    HANDLE e = CreateEventA(NULL, TRUE, TRUE, NULL);         /* manual-reset, signaled */
    printf("waitex=%lu\n", (unsigned long)WaitForSingleObjectEx(e, 1000, FALSE));  /* 0 */
    CloseHandle(e);

    printf("done\n");
    return 0;
}
