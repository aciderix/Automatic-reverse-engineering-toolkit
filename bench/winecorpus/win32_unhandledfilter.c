/* SetUnhandledExceptionFilter is stateful: it returns the previously installed
 * top-level filter, so a program that saves and restores it round-trips. The absolute
 * first value differs by design (Wine has a default filter pre-registered, ARET starts
 * with none), so the fixture tests the RELATIVE chain (set A -> set B returns A -> set
 * NULL returns B), which is environment-independent. Expected identical Wine and ARET. */
#include <windows.h>
#include <stdio.h>
static LONG WINAPI fa(EXCEPTION_POINTERS *p) { (void)p; return EXCEPTION_EXECUTE_HANDLER; }
static LONG WINAPI fb(EXCEPTION_POINTERS *p) { (void)p; return EXCEPTION_EXECUTE_HANDLER; }
int main(void) {
    SetUnhandledExceptionFilter(fa);                              /* prime (ignore result) */
    LPTOP_LEVEL_EXCEPTION_FILTER p1 = SetUnhandledExceptionFilter(fb);
    LPTOP_LEVEL_EXCEPTION_FILTER p2 = SetUnhandledExceptionFilter(NULL);
    printf("p1_is_fa=%d p2_is_fb=%d\n", p1 == fa, p2 == fb);
    return 0;
}
