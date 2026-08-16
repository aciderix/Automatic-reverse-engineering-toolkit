/* Vectored Exception Handling (AddVectoredExceptionHandler / RemoveVectoredException
 * Handler) — the FIRST-CHANCE handler chain that runs ahead of frame-based SEH. glib
 * and libwinpthread install one at startup (the wall that blocked the real lifted
 * libglib gspawn helper). Deterministic and bit-comparable: register a VEH, raise a
 * software exception, the handler sees the exact code/flags and asks CONTINUE_EXECUTION
 * so control resumes right after RaiseException (a CONTINUE_SEARCH with no other
 * handler would terminate — untestable cleanly), then unregister. */
#include <windows.h>
#include <stdio.h>

static int hits = 0;

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    hits++;
    printf("veh code=%08lx flags=%lu params=%lu\n",
           ep->ExceptionRecord->ExceptionCode,
           ep->ExceptionRecord->ExceptionFlags,
           ep->ExceptionRecord->NumberParameters);
    return EXCEPTION_CONTINUE_EXECUTION;
}

int main(void) {
    PVOID h = AddVectoredExceptionHandler(1, veh);
    printf("added=%d\n", h != NULL);

    RaiseException(0xDEADBEEF, 0, 0, NULL);
    printf("after raise, hits=%d\n", hits);          /* reached iff the VEH continued */

    ULONG r = RemoveVectoredExceptionHandler(h);
    printf("removed=%d\n", r != 0);
    printf("done\n");
    return 0;
}
