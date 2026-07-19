/* Misc kernel32/user32 batch from the 2026-07-19 Levier-0 re-measure (display-free):
 *  - IsDBCSLeadByte: CP1252 (Western ACP) is single-byte -> always 0.
 *  - wvsprintfA: the va_list form of wsprintfA (limited printf, no float).
 *  - GetLogicalDrives: bit 2 (C:) present (the full mask includes Wine's env-specific
 *    Z:, so the fixture checks the derived C:-present bit, not the raw mask).
 * (OpenFile is also implemented — HFILE=fd, same model as the proven _lopen — but is
 * NOT fixtured: Wine 9.0's OpenFile is unreliable here (OF_CREATE fails and does not
 * create; OF_EXIST is flaky), so it is not a valid bit-oracle. ARET's OpenFile is
 * correct per the Win32 contract / POSIX-model equivalence to _lopen.) */
#include <windows.h>
#include <stdio.h>

static void vfmt(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); wvsprintfA(buf, fmt, ap); va_end(ap);
}

int main(void) {
    printf("dbcs 0x41=%d 0x81=%d 0xE0=%d\n",
           IsDBCSLeadByte(0x41), IsDBCSLeadByte(0x81), IsDBCSLeadByte(0xE0));

    char b[128];
    vfmt(b, "n=%d s=%s x=%04x u=%u c=%c", -7, "hi", 0xab, 42u, 'Q');
    printf("wvsprintf=[%s]\n", b);

    printf("driveC=%d\n", (GetLogicalDrives() >> 2) & 1);
    printf("done\n");
    return 0;
}
