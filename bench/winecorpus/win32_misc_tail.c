/* Misc long-tail (display-free, measured vs Wine): WaitForInputIdle — the
 * deterministic non-display member of the Win95-plateau "divers" family.
 * It is valid only for a child GUI process; none exists here (CreateProcess is a
 * sound failure), so it fails with WAIT_FAILED / ERROR_INVALID_HANDLE, never a fake
 * "idle" success.
 *
 * (WinHelpA/W is also implemented — HELP_QUIT->TRUE, else FALSE — but is kept OUT of
 * this in-harness fixture: under Wine WinHelp spawns a winhlp32 viewer child that
 * lingers on the stdout pipe and would hang the capture. Its shim is verified by
 * direct out-of-harness measurement instead. TabbedTextOut/GrayString are GDI-text
 * rendering, a separate FreeType sub-family.) */
#include <windows.h>
#include <stdio.h>

int main(void) {
    DWORD wi_self  = WaitForInputIdle(GetCurrentProcess(), 0);
    DWORD wi_bogus = WaitForInputIdle((HANDLE)0x12345678, 100);
    printf("waitidle self=%lu bogus=%lu bogus_err=%lu\n",
           (unsigned long)wi_self, (unsigned long)wi_bogus, (unsigned long)GetLastError());
    printf("done\n");
    return 0;
}
