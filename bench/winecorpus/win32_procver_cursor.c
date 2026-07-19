/* Startup-path imports surfaced by running a real Win32 GUI binary (FishTank.exe, a
 * Win95 comctl32 app) under ARET — the step-3 "real binary drives the shims" method.
 * These three were the only unimplemented imports on FishTank's run path; with them
 * it reaches its message loop like Wine. All display-free and deterministic:
 *  - GetProcessVersion: the Windows version the process targets, 0x00040000 (Win4.0).
 *  - SetMessageQueue: obsolete Win16 no-op -> TRUE.
 *  - GetCursorPos: fills the POINT and returns TRUE (the mouse position itself is
 *    env-dependent, so only the return is compared, not x/y). */
#include <windows.h>
#include <stdio.h>

int main(void) {
    printf("procver=%08lx\n", (unsigned long)GetProcessVersion(0));
    printf("setmsgqueue=%d\n", SetMessageQueue(96));
    POINT p = { 111, 222 };
    BOOL gc = GetCursorPos(&p);
    printf("getcursorpos ret=%d\n", gc);
    printf("done\n");
    return 0;
}
