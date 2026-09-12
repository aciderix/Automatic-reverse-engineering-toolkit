/* AdjustWindowRectEx(RECT*, style, bMenu, exStyle) computes the WINDOW rect that yields a
 * given CLIENT rect, i.e. it inflates by the non-client frame: borders, caption bar, and —
 * when bMenu — one menu-bar row. A borderless WS_POPUP is left unchanged; every decorated
 * style grows. notepad (and nearly every SDI app) calls it to size its top-level window from
 * a desired client size, so a no-op here makes the window too small by the whole frame. The
 * exact deltas are Wine's non-client metrics — this fixture prints them for winediff to
 * compare against the real user32, never a guessed constant. Headless (pure geometry). */
#include <windows.h>
#include <stdio.h>

static void probe(const char *tag, DWORD style, BOOL menu, DWORD ex) {
    RECT r = { 100, 100, 500, 400 };   /* a 400x300 client at (100,100) */
    BOOL ok = AdjustWindowRectEx(&r, style, menu, ex);
    printf("%-20s ok=%d rect=(%ld,%ld,%ld,%ld) wh=%ldx%ld\n",
           tag, ok, r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top);
}

int main(void) {
    probe("popup",          WS_POPUP,            FALSE, 0);                 /* borderless: unchanged */
    probe("caption",        WS_CAPTION,          FALSE, 0);                 /* +border +caption */
    probe("overlappedwin",  WS_OVERLAPPEDWINDOW, FALSE, 0);                 /* thick frame +caption */
    probe("overlapped+menu",WS_OVERLAPPEDWINDOW, TRUE,  0);                 /* + one menu row */
    probe("caption+menu",   WS_CAPTION,          TRUE,  0);
    probe("thickframe",     WS_THICKFRAME,       FALSE, 0);                 /* resize border, no caption */
    probe("border",         WS_BORDER,           FALSE, 0);                 /* thin line border */
    probe("dlgframe",       WS_DLGFRAME,         FALSE, 0);                 /* dialog frame, no caption text */
    probe("clientedge",     WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CLIENTEDGE);  /* + sunken inner edge */
    probe("wdwedge",        WS_OVERLAPPEDWINDOW, FALSE, WS_EX_WINDOWEDGE);
    probe("toolwindow",     WS_CAPTION,          FALSE, WS_EX_TOOLWINDOW);  /* small caption */
    {   /* AdjustWindowRect (no-Ex) must equal the Ex variant with ex=0. */
        RECT a = { 0, 0, 300, 200 }, b = { 0, 0, 300, 200 };
        AdjustWindowRect(&a, WS_OVERLAPPEDWINDOW, TRUE);
        AdjustWindowRectEx(&b, WS_OVERLAPPEDWINDOW, TRUE, 0);
        printf("noex==ex %d\n", a.left==b.left && a.top==b.top && a.right==b.right && a.bottom==b.bottom);
    }
    /* The raw non-client metrics the frame is built from — proven directly so the table
     * and AdjustWindowRectEx cannot silently disagree with GetSystemMetrics. */
    printf("SM CYCAPTION=%d CYSMCAPTION=%d CYMENU=%d CXFRAME=%d CYFRAME=%d "
           "CXDLGFRAME=%d CYDLGFRAME=%d CXBORDER=%d CYBORDER=%d CXEDGE=%d CYEDGE=%d\n",
           GetSystemMetrics(SM_CYCAPTION), GetSystemMetrics(SM_CYSMCAPTION),
           GetSystemMetrics(SM_CYMENU), GetSystemMetrics(SM_CXFRAME),
           GetSystemMetrics(SM_CYFRAME), GetSystemMetrics(SM_CXDLGFRAME),
           GetSystemMetrics(SM_CYDLGFRAME), GetSystemMetrics(SM_CXBORDER),
           GetSystemMetrics(SM_CYBORDER), GetSystemMetrics(SM_CXEDGE),
           GetSystemMetrics(SM_CYEDGE));
    printf("done\n");
    return 0;
}
