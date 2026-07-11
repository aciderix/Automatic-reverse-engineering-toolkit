/* G2b (doc 72) — a *visible* top-level window, presented via SDL2 on ARET and by
 * a real window on Wine. The oracle is the deterministic *content*: the client
 * rect of a borderless popup, and GDI drawing into the window DC read back — both
 * bit-identical to Wine, headless (SDL_VIDEODRIVER=dummy / Xvfb). The pixels on
 * screen are the architectural payoff; what we *verify* is that GetDC(hwnd) binds
 * a real client surface the program can draw into and read back, exactly as Wine.
 *
 * A borderless WS_POPUP window is used so the client rect equals the window rect
 * (no non-client frame to model), keeping GetClientRect deterministic across
 * window managers. */
#include <windows.h>
#include <stdio.h>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProc(h, m, w, l);
}

int main(void) {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = "AretG2b";
    ATOM a = RegisterClassA(&wc);
    printf("class=%d\n", a ? 1 : 0);

    HWND hw = CreateWindowExA(0, "AretG2b", "g2b", WS_POPUP | WS_VISIBLE,
                              0, 0, 40, 30, NULL, NULL, NULL, NULL);
    printf("win=%d\n", hw ? 1 : 0);

    RECT rc;
    GetClientRect(hw, &rc);
    printf("client=%ldx%ld\n", (long)(rc.right - rc.left), (long)(rc.bottom - rc.top));

    /* Draw into the window's client DC and read it straight back. */
    HDC dc = GetDC(hw);
    SetPixel(dc, 1, 1, RGB(10, 20, 30));
    SetPixel(dc, 7, 4, RGB(200, 100, 50));
    SetPixel(dc, 39, 29, RGB(1, 2, 3));       /* bottom-right corner */
    COLORREF c1 = GetPixel(dc, 1, 1);
    COLORREF c2 = GetPixel(dc, 7, 4);
    COLORREF c3 = GetPixel(dc, 39, 29);
    printf("px=%06lX,%06lX,%06lX\n",
           (unsigned long)c1, (unsigned long)c2, (unsigned long)c3);
    ReleaseDC(hw, dc);

    /* A paint cycle: bind the DC, draw, present. Deterministic output = the pixel
     * we then read back through a fresh window DC (the paint persisted). */
    PAINTSTRUCT ps;
    HDC pdc = BeginPaint(hw, &ps);
    SetPixel(pdc, 3, 3, RGB(90, 80, 70));
    EndPaint(hw, &ps);
    HDC dc2 = GetDC(hw);
    printf("paint=%06lX\n", (unsigned long)GetPixel(dc2, 3, 3));
    ReleaseDC(hw, dc2);

    DestroyWindow(hw);
    printf("done\n");
    return 0;
}
