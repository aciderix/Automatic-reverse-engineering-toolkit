/* Window-manager state from the deep-GUI cluster (2026-07-19 re-measure): mouse
 * capture and scroll bars. Both are pure state — no drawing — so they round-trip
 * deterministically and match Wine bit-for-bit (no display content compared, but a
 * window must exist, so this uses the shared Xvfb like the other GUI fixtures).
 *  - SetCapture returns the previous holder, GetCapture the current, ReleaseCapture
 *    clears it.
 *  - SetScrollPos clamps to [min,max] and returns the previous pos; SetScrollInfo
 *    with a page clamps pos to [nMin, nMax-nPage+1]; nTrackPos = current pos. */
#include <windows.h>
#include <stdio.h>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProc(h, m, w, l); }

int main(void) {
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "CapScroll";
    RegisterClassA(&wc);
    HWND h = CreateWindowExA(0, "CapScroll", "t", WS_OVERLAPPEDWINDOW | WS_HSCROLL | WS_VSCROLL,
                             0, 0, 200, 200, NULL, NULL, wc.hInstance, NULL);
    printf("hwnd=%d\n", h != NULL);

    HWND c0 = GetCapture();
    HWND p1 = SetCapture(h);
    HWND c1 = GetCapture();
    BOOL rel = ReleaseCapture();
    HWND c2 = GetCapture();
    printf("cap c0=%d prev=%d cur_is_h=%d rel=%d after=%d\n",
           c0 != NULL, p1 != NULL, c1 == h, rel, c2 != NULL);

    SetScrollRange(h, SB_HORZ, 10, 100, FALSE);
    int mn = 0, mx = 0; GetScrollRange(h, SB_HORZ, &mn, &mx);
    int prev  = SetScrollPos(h, SB_HORZ, 50, FALSE);  int pos  = GetScrollPos(h, SB_HORZ);
    int prev2 = SetScrollPos(h, SB_HORZ, 999, FALSE);  int pos2 = GetScrollPos(h, SB_HORZ);
    int prev3 = SetScrollPos(h, SB_HORZ, -5, FALSE);   int pos3 = GetScrollPos(h, SB_HORZ);
    printf("range mn=%d mx=%d | prev=%d pos=%d | hi prev=%d pos=%d | lo prev=%d pos=%d\n",
           mn, mx, prev, pos, prev2, pos2, prev3, pos3);

    SCROLLINFO si; memset(&si, 0, sizeof si);
    si.cbSize = sizeof si; si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0; si.nMax = 100; si.nPage = 20; si.nPos = 95;   /* clamps to 81 */
    SetScrollInfo(h, SB_VERT, &si, FALSE);
    SCROLLINFO go; memset(&go, 0, sizeof go);
    go.cbSize = sizeof go; go.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_TRACKPOS;
    GetScrollInfo(h, SB_VERT, &go);
    printf("si min=%d max=%d page=%d pos=%d track=%d\n", go.nMin, go.nMax, go.nPage, go.nPos, go.nTrackPos);
    printf("done\n");
    return 0;
}
