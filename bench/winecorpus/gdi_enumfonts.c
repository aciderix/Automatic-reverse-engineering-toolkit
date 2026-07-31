/* EnumFontFamilies(A/W) — enumerate installed font families through a callback (what a
 * font picker does; the wall real MFC/WinMerge hits after its non-client metrics).
 *
 * What this fixture asserts is the CONTRACT, which is deterministic on any machine —
 * NOT the list of fonts, which is environment-dependent (it is the host's installed
 * set, several hundred families here, and equally environment-dependent under Wine).
 * Printing the families, their count or their metrics would make this test machine-
 * specific rather than a specification, so it deliberately checks only:
 *   - a family that cannot exist yields ZERO callbacks and still returns 1;
 *   - a callback returning 0 stops the enumeration at once, and the function returns
 *     that 0 — not a count (a real trap: "returns the last callback value");
 *   - a callback returning non-zero enumerates to the end and returns 1;
 *   - at least one family exists (true under Wine and under ARET, both of which read
 *     the host's fonts through fontconfig), every callback gets a non-empty face name,
 *     and the LOGFONT/TEXTMETRIC pitch-and-family bytes hold their real relationship:
 *     the SAME FF_* family nibble but DIFFERENT low bits (the LOGFONT carries the
 *     pitch request, the TEXTMETRIC the TMPF_* flags — measured lf 0x22 vs tm 0x27);
 *   - the A and W entry points behave identically.
 * These are reported as booleans over all callbacks, never as counts: a count would be
 * the number of installed families and so machine-specific. Expected identical under
 * Wine and ARET. */
#include <windows.h>
#include <stdio.h>

static int g_calls, g_stop_after, g_bad_face, g_pf_same_byte, g_pf_diff_family;

static void note_pf(unsigned lfpf, unsigned tmpf) {
    if (lfpf == tmpf) g_pf_same_byte++;                    /* must never happen   */
    if ((lfpf & 0xf0) != (tmpf & 0xf0)) g_pf_diff_family++; /* must never happen   */
}
static int CALLBACK cbW(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lp) {
    g_calls++;
    if (!lf->lfFaceName[0]) g_bad_face++;
    note_pf((unsigned)lf->lfPitchAndFamily, (unsigned)tm->tmPitchAndFamily);
    (void)type; (void)lp;
    return (g_stop_after && g_calls >= g_stop_after) ? 0 : 1;
}
static int CALLBACK cbA(const LOGFONTA *lf, const TEXTMETRICA *tm, DWORD type, LPARAM lp) {
    g_calls++;
    if (!lf->lfFaceName[0]) g_bad_face++;
    note_pf((unsigned)lf->lfPitchAndFamily, (unsigned)tm->tmPitchAndFamily);
    (void)type; (void)lp;
    return (g_stop_after && g_calls >= g_stop_after) ? 0 : 1;
}

int main(void) {
    HDC dc = GetDC(NULL);

    /* A family that cannot exist: no callback, still TRUE. */
    g_calls = 0; g_stop_after = 0;
    int r = EnumFontFamiliesW(dc, L"NoSuchFamily_ZZ_42", cbW, 0);
    printf("missing   ret=%d calls=%d\n", r, g_calls);

    /* Enumerate everything: TRUE, and there is at least one family. */
    g_calls = 0; g_stop_after = 0; g_bad_face = 0; g_pf_same_byte = 0; g_pf_diff_family = 0;
    r = EnumFontFamiliesW(dc, NULL, cbW, 0);
    printf("all-W     ret=%d any=%d badface=%d pf_same=%d pf_famdiff=%d\n",
           r, g_calls > 0, g_bad_face, g_pf_same_byte, g_pf_diff_family);

    /* Stop on the first callback: exactly one call, and the RETURN IS 0. */
    g_calls = 0; g_stop_after = 1;
    r = EnumFontFamiliesW(dc, NULL, cbW, 0);
    printf("stop1-W   ret=%d calls=%d\n", r, g_calls);

    /* Same through the A entry point. */
    g_calls = 0; g_stop_after = 0; g_bad_face = 0; g_pf_same_byte = 0; g_pf_diff_family = 0;
    r = EnumFontFamiliesA(dc, NULL, cbA, 0);
    printf("all-A     ret=%d any=%d badface=%d pf_same=%d pf_famdiff=%d\n",
           r, g_calls > 0, g_bad_face, g_pf_same_byte, g_pf_diff_family);

    g_calls = 0; g_stop_after = 1;
    r = EnumFontFamiliesA(dc, NULL, cbA, 0);
    printf("stop1-A   ret=%d calls=%d\n", r, g_calls);

    g_calls = 0; g_stop_after = 0;
    r = EnumFontFamiliesA(dc, "NoSuchFamily_ZZ_42", cbA, 0);
    printf("missing-A ret=%d calls=%d\n", r, g_calls);

    /* lParam is passed through untouched. */
    ReleaseDC(NULL, dc);
    return 0;
}
