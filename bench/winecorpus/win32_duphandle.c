/* DuplicateHandle on the GetCurrentThread() pseudo-handle. This is what mingw's
 * libwinpthread does during thread/pthread init: it turns the -2 current-thread
 * pseudo-handle into a REAL, distinct, waitable/closeable handle. ARET used to dup()
 * the value as if it were a host fd (dup(-2) fails -> FALSE), which made a lifted
 * libwinpthread abort on the first C++ throw. The fix resolves the pseudo to a real
 * per-fiber thread handle. All asserts are booleans/constants (the handle VALUE
 * differs between ARET and Wine, so it is never printed). */
#include <windows.h>
#include <stdio.h>

int main(void) {
    HANDLE dupT = NULL, dupP = NULL;
    BOOL okT = DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                               GetCurrentProcess(), &dupT, 0, FALSE, DUPLICATE_SAME_ACCESS);
    /* a real, non-null handle, distinct from the -2 pseudo */
    printf("thread ok=%d nonnull=%d not_pseudo=%d\n",
           okT, dupT != NULL, dupT != GetCurrentThread());
    /* the calling thread is running, so its handle is not signaled -> WAIT_TIMEOUT */
    DWORD w = WaitForSingleObject(dupT, 0);
    printf("wait timeout=%d\n", w == WAIT_TIMEOUT);
    printf("close=%d\n", CloseHandle(dupT));

    BOOL okP = DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(),
                               GetCurrentProcess(), &dupP, 0, FALSE, DUPLICATE_SAME_ACCESS);
    printf("proc ok=%d nonnull=%d\n", okP, dupP != NULL);
    return 0;
}
