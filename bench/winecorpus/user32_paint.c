/* G2b paint model (doc 72) — the invalidation -> WM_PAINT flow that makes a
 * visible window actually show app-driven content. A window's WNDPROC paints in
 * its WM_PAINT handler (BeginPaint/SetPixel/EndPaint); the app forces delivery
 * with UpdateWindow and drains a generated WM_PAINT through PeekMessage. Every
 * observed fact (paint counts, the pixel drawn, update-region state) is
 * deterministic and bit-identical to Wine, headless (SDL dummy / Xvfb).
 *
 * WS_POPUP so the client rect equals the window (no non-client frame to model). */
#include <windows.h>
#include <stdio.h>

static int g_paints = 0;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        SetPixel(dc, 4, 4, RGB(11, 22, 33));
        g_paints++;
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

int main(void) {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = "AretPaint";
    RegisterClassA(&wc);

    HWND hw = CreateWindowExA(0, "AretPaint", "p", WS_POPUP | WS_VISIBLE,
                              0, 0, 32, 24, NULL, NULL, NULL, NULL);

    /* A freshly shown window owes a WM_PAINT; UpdateWindow delivers it now. */
    UpdateWindow(hw);
    printf("after_update=%d\n", g_paints);

    /* The paint persisted into the client surface. */
    HDC dc = GetDC(hw);
    printf("px=%06lX\n", (unsigned long)GetPixel(dc, 4, 4));
    ReleaseDC(hw, dc);

    /* Invalidate again, then drain the generated WM_PAINT via PeekMessage. Only
     * the paint count is observed: mouse/keyboard events are real input, which is
     * environment-dependent and non-deterministic (doc 72 §4 — content, not
     * input), so the loop iteration count is deliberately not compared. */
    InvalidateRect(hw, NULL, FALSE);
    int guard = 0;
    MSG m;
    while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) {
        DispatchMessage(&m);
        if (++guard > 16) break;
    }
    printf("after_peek=%d\n", g_paints);

    /* Update region now empty -> UpdateWindow is a no-op, no extra paint. */
    UpdateWindow(hw);
    printf("after_noop=%d\n", g_paints);

    DestroyWindow(hw);
    printf("done\n");
    return 0;
}
