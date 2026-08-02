/* CRT + console + handle cluster (widest HLE gaps on the CLI corpus, per wallsweep):
 * _getmaxstdio/_setmaxstdio, _dup/_dup2, GetHandleInformation/SetHandleInformation,
 * DuplicateHandle, SetConsoleTextAttribute, raise (installed handler), __argc/__argv.
 *
 * Values that are environment-specific (the exact fd number, handle value, flag bits)
 * are NOT printed — only the invariant contract, so the fixture is bit-identical Wine.
 * SetConsoleTextAttribute's return is faithful (isatty): under winediff stdout is a
 * pipe, so both ARET and Wine report "not a console" (0). */
#include <windows.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

static volatile int g_raised = 0;
static void on_sig(int s) { g_raised = s; }

int main(void) {
    /* _getmaxstdio / _setmaxstdio */
    int m0 = _getmaxstdio();
    int m1 = _setmaxstdio(1024);
    int m2 = _getmaxstdio();
    printf("maxstdio def>=512:%d set:%d get:%d\n", m0 >= 512, m1, m2);

    /* _dup / _dup2 (redirection is genuine; exact fd is env-specific -> booleans) */
    int fd = _dup(1);
    printf("dup ok:%d\n", fd >= 0);
    if (fd >= 0) {
        int r = _dup2(fd, fd);      /* dup2 onto itself -> 0 */
        printf("dup2 self:%d\n", r);
        _close(fd);
    }

    /* handle info + duplicate */
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD flags = 0xdeadbeef;
    BOOL gi = GetHandleInformation(hOut, &flags);
    printf("gethandleinfo ok:%d\n", gi);
    HANDLE hDup = NULL;
    BOOL dh = DuplicateHandle(GetCurrentProcess(), hOut, GetCurrentProcess(),
                              &hDup, 0, FALSE, DUPLICATE_SAME_ACCESS);
    printf("duphandle ok:%d nonnull:%d\n", dh, hDup != NULL);
    if (hDup) CloseHandle(hDup);
    BOOL sh = SetHandleInformation(hOut, HANDLE_FLAG_INHERIT, 0);
    printf("sethandleinfo ok:%d\n", sh);

    /* console colour: faithful return (0 under redirection, both sides) */
    BOOL sc = SetConsoleTextAttribute(hOut, 7);
    printf("setconsoleattr:%d\n", sc);

    /* raise through an installed handler (deterministic, one-shot) */
    signal(SIGABRT, on_sig);
    int rr = raise(SIGABRT);
    printf("raise ret:%d handler:%d\n", rr, g_raised == SIGABRT);

    /* __argc / __argv */
    printf("argc>=1:%d argv0:%d\n", __argc >= 1, __argv && __argv[0] != NULL);
    return 0;
}
