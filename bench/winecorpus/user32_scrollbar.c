/* Non-client scrollbar STRUCTURE (theme-independent) — measured vs Wine.
 *
 * A window with WS_VSCROLL|WS_HSCROLL gets its scrollbars drawn by user32 in the
 * NON-CLIENT area. The COLORS are a theme decision (ARET = classic Win95 grey,
 * Wine-oracle = modern) and are deliberately NOT pixel-compared (KN-0112). But the
 * STRUCTURE — the scrollbar rectangle, the arrow-button size, and the thumb's
 * top/bottom computed from the scroll range/page/pos — is geometry: it is the SAME
 * whatever the palette, so it CAN be proven bit-exact. Win32 exposes it via
 * GetScrollBarInfo(SCROLLBARINFO) + GetSystemMetrics, which is what we compare here.
 *
 * Headless: we create the window and query the model, we never rely on pixels. The
 * scrollbar rect is reported RELATIVE to the window origin so it is independent of
 * where the window manager places the window. Proven against Wine via winediff. */
#include <windows.h>
#include <stdio.h>

#ifndef OBJID_VSCROLL
#define OBJID_VSCROLL 0xFFFFFFFB
#define OBJID_HSCROLL 0xFFFFFFFA
#endif

static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

/* rcScrollBar comes back in screen coords; report it relative to the window rect so the
 * numbers are position-independent (window-manager placement must not change the test). */
static void report_sb(const char *tag, HWND h, DWORD objid) {
    RECT wr; GetWindowRect(h, &wr);
    SCROLLBARINFO sbi; sbi.cbSize = sizeof(sbi);
    if (!GetScrollBarInfo(h, objid, &sbi)) { printf("%-8s GetScrollBarInfo=FAIL\n", tag); return; }
    printf("%-8s rc=(%ld,%ld,%ld,%ld) dxyLine=%d thumbTop=%d thumbBot=%d state0=0x%lx\n",
           tag,
           (long)(sbi.rcScrollBar.left   - wr.left), (long)(sbi.rcScrollBar.top    - wr.top),
           (long)(sbi.rcScrollBar.right  - wr.left), (long)(sbi.rcScrollBar.bottom - wr.top),
           (int)sbi.dxyLineButton, (int)sbi.xyThumbTop, (int)sbi.xyThumbBottom,
           (unsigned long)sbi.rgstate[0]);
}

int main(void) {
    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wp; wc.hInstance = hi; wc.lpszClassName = L"AretScroll";
    RegisterClassW(&wc);

    printf("SM_CXVSCROLL=%d SM_CYVSCROLL=%d SM_CYHSCROLL=%d SM_CXHSCROLL=%d SM_CXHTHUMB=%d SM_CYVTHUMB=%d\n",
           GetSystemMetrics(SM_CXVSCROLL), GetSystemMetrics(SM_CYVSCROLL),
           GetSystemMetrics(SM_CYHSCROLL), GetSystemMetrics(SM_CXHSCROLL),
           GetSystemMetrics(SM_CXHTHUMB), GetSystemMetrics(SM_CYVTHUMB));

    HWND h = CreateWindowExW(0, L"AretScroll", L"sb",
                             WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL,
                             0, 0, 320, 240, NULL, NULL, hi, NULL);
    if (!h) { printf("CreateWindow FAIL err=%lu\n", (unsigned long)GetLastError()); return 1; }

    /* Deterministic scroll state so the thumb geometry is a fixed function of range/page/pos. */
    SCROLLINFO si; si.cbSize = sizeof(si); si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0; si.nMax = 99; si.nPage = 20; si.nPos = 30; si.nTrackPos = 0;
    SetScrollInfo(h, SB_VERT, &si, FALSE);
    si.nMin = 0; si.nMax = 199; si.nPage = 50; si.nPos = 40;
    SetScrollInfo(h, SB_HORZ, &si, FALSE);

    report_sb("vscroll", h, OBJID_VSCROLL);
    report_sb("hscroll", h, OBJID_HSCROLL);

    DestroyWindow(h);
    printf("done\n");
    return 0;
}
