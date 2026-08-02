/* StrFromTimeIntervalW/A [SHLWAPI] — MEDIUM-form Wine body port (doc 82).
 * Exercises the significant-digit truncation across hours/minutes/seconds classes,
 * the leading-space quirk, the cchMax clamp, and the A variant's always-0 return.
 * Verifies bit-identical Wine. */
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>

static void w(DWORD ms, int digits, UINT cchMax) {
    WCHAR buf[128];
    for (int i = 0; i < 128; i++) buf[i] = 0x2323;   /* poison to expose over/under-writes */
    INT r = StrFromTimeIntervalW(buf, cchMax, ms, digits);
    char nar[256]; int j = 0;
    for (int i = 0; buf[i] && i < 200; i++) nar[j++] = (char)buf[i];
    nar[j] = 0;
    printf("W ms=%lu d=%d cch=%u -> ret=%d \"%s\"\n", (unsigned long)ms, digits, cchMax, (int)r, nar);
}

static void a(DWORD ms, int digits, UINT cchMax) {
    char buf[128];
    for (int i = 0; i < 128; i++) buf[i] = 0x23;
    INT r = StrFromTimeIntervalA(buf, cchMax, ms, digits);
    buf[127] = 0;
    printf("A ms=%lu d=%d cch=%u -> ret=%d \"%s\"\n", (unsigned long)ms, digits, cchMax, (int)r, buf);
}

int main(void) {
    /* Wine's documented example: 138h 43m 15s across iDigits 1..7 */
    DWORD ex = (138u * 3600u + 43u * 60u + 15u) * 1000u;
    for (int d = 1; d <= 7; d++) w(ex, d, 64);

    w(0, 3, 64);              /* zero -> " 0 sec" */
    w(500, 3, 64);            /* rounds up to 1 s */
    w(499, 3, 64);            /* rounds down to 0 s */
    w(59000, 3, 64);          /* 59 sec, no minute class */
    w(60000, 3, 64);          /* 1 min */
    w(3600000, 3, 64);        /* 1 hr */
    w(3661000, 5, 64);        /* 1 hr 1 min 1 sec, plenty of digits */
    w(3661000, 2, 64);        /* truncated significance */

    /* degenerate cchMax / iDigits */
    w(ex, 0, 64);             /* iDigits 0 -> empty, ret 0 */
    w(ex, 3, 1);              /* cchMax 1 -> empty, ret 0 */
    w(ex, 4, 8);              /* clamp mid-string */
    w(ex, 4, 6);              /* tight clamp */

    /* A variant: content narrows, return is ALWAYS 0 (Wine quirk) */
    a(ex, 3, 64);
    a(60000, 2, 64);
    a(3661000, 5, 10);        /* clamp on the narrow path */
    return 0;
}
