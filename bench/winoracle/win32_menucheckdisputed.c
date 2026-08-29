/* SM_CXMENUCHECK / SM_CYMENUCHECK — the menu check-mark dimensions, a cell where
 * ARET and Wine disagree and only real Windows can settle it.
 *
 * winecorpus/user32_menu2.c is measured against Wine and, on this Wine build, the
 * gate reads:
 *
 *     cxcheck=11 cycheck=11        (Wine)      vs      cxcheck=13 cycheck=13   (ARET)
 *
 * ARET's user32 shim returns 13 (runtime/aret_hle/aret_win32.c: SM_CXMENUCHECK /
 * SM_CYMENUCHECK), the value the fixture's own comment records as the documented
 * default ("default 13x13"). This Wine build returns 11. Wine is the suspect here,
 * exactly the circularity the doctrine records (70 §1): where Wine is both oracle and
 * reference, a Wine/ARET disagreement cannot be settled by Wine. So this probe asks
 * the real Win32 the original binaries were built against.
 *
 * GetMenuCheckMarkDimensions() is the legacy accessor and is documented as
 * MAKELONG(SM_CXMENUCHECK, SM_CYMENUCHECK); printing all three proves they agree.
 *
 * A few neighbouring metrics are printed for context only — enough to show the runner
 * has a real metric set (not a degenerate all-zero headless desktop) without turning
 * this into a metrics dump. Only the check-mark values are the finding.
 *
 * NOT a gate (bench/winoracle/README.md): this prints a measurement a session reads
 * and then encodes. If real Windows prints 13, ARET is right and the Wine gate entry
 * for user32_menu2 is a proven oracle divergence. If it prints something else, ARET's
 * hard-coded 13 is a guess to correct (§0: rien de prouvé = rien de deviné). */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    int cx = GetSystemMetrics(SM_CXMENUCHECK);   /* index 71 */
    int cy = GetSystemMetrics(SM_CYMENUCHECK);   /* index 72 */
    DWORD d = GetMenuCheckMarkDimensions();
    printf("SM_CXMENUCHECK=%d SM_CYMENUCHECK=%d\n", cx, cy);
    printf("GetMenuCheckMarkDimensions=%08lx cx=%d cy=%d\n",
           (unsigned long)d, (int)LOWORD(d), (int)HIWORD(d));

    /* Context: menu bar/button metrics, so a degenerate headless desktop (all zero)
     * is visible rather than mistaken for a real disagreement. */
    printf("context SM_CXMENUSIZE=%d SM_CYMENUSIZE=%d SM_CYMENU=%d SM_CXBORDER=%d\n",
           GetSystemMetrics(SM_CXMENUSIZE), GetSystemMetrics(SM_CYMENUSIZE),
           GetSystemMetrics(SM_CYMENU), GetSystemMetrics(SM_CXBORDER));
    return 0;
}
