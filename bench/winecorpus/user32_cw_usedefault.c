/* CW_USEDEFAULT placement (user32), measured against Wine on 5 screen sizes including
   non-exact divisions (1001x701 -> 750x525, 803x603 -> 602x452): for an OVERLAPPED
   window the OS fills the window rect to THREE QUARTERS of the work area, truncated —
   right = wa.left + (wa.w*3)/4, bottom = wa.top + (wa.h*3)/4 — and a CW_USEDEFAULT
   position lands at the work-area origin. An explicit position keeps the SAME right/
   bottom (so cx = right - x), i.e. the default is a bound, not a size. WS_POPUP /
   WS_CHILD get 0x0 instead. Before this, ARET mapped CW_USEDEFAULT to 0 -> every such
   window was 0x0 (silently wrong, §0.1).

   Screen-size independent BY CONSTRUCTION: Wine reports the real X screen while ARET
   keeps its documented virtual 1024x768 screen invariant (doc 72 4.5), so absolute
   pixel sizes are NOT comparable. The fixture therefore re-reads SPI_GETWORKAREA and
   prints only DELTAS against the recomputed formula (all zero) plus booleans — those
   are identical on both engines iff the formula itself matches. Only GetWindowRect is
   asserted: the client rect legitimately differs (Wine has a frame, ARET is frameless). */
#include <windows.h>
#include <stdio.h>

static RECT g_wa;
static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

/* Print the window rect as deltas from the measured Wine formula (all 0 when it holds). */
static void probe(const char *tag, HWND h, int expx, int expy, int expright, int expbottom) {
    RECT r = {0,0,0,0};
    GetWindowRect(h, &r);
    printf("%s dx=%ld dy=%ld dright=%ld dbottom=%ld\n", tag,
           (long)(r.left - expx), (long)(r.top - expy),
           (long)(r.right - expright), (long)(r.bottom - expbottom));
}

int main(void) {
    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0}; wc.lpfnWndProc = wp; wc.hInstance = hi; wc.lpszClassName = L"CwDef";
    RegisterClassW(&wc);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &g_wa, 0);
    /* The measured rule, recomputed from THIS engine's own work area. */
    int R = g_wa.left + ((g_wa.right  - g_wa.left) * 3) / 4;
    int B = g_wa.top  + ((g_wa.bottom - g_wa.top)  * 3) / 4;
    printf("wa_nonempty=%d\n", (g_wa.right > g_wa.left && g_wa.bottom > g_wa.top));

    /* Default position AND default size: origin at the work area, bound at 3/4. */
    HWND a = CreateWindowExW(0, L"CwDef", L"t", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                             NULL, NULL, hi, NULL);
    probe("ovl", a, g_wa.left, g_wa.top, R, B);

    /* Explicit position, default size: right/bottom UNCHANGED (a bound, not a size). */
    HWND b = CreateWindowExW(0, L"CwDef", L"t", WS_OVERLAPPEDWINDOW,
                             10, 20, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hi, NULL);
    probe("posdef", b, 10, 20, R, B);

    /* Default position, explicit size: exact size at the work-area origin. */
    HWND c = CreateWindowExW(0, L"CwDef", L"t", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 300, 200, NULL, NULL, hi, NULL);
    probe("defpos", c, g_wa.left, g_wa.top, g_wa.left + 300, g_wa.top + 200);

    /* WS_POPUP: CW_USEDEFAULT collapses to 0 (position and size). */
    HWND d = CreateWindowExW(0, L"CwDef", L"t", WS_POPUP,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                             NULL, NULL, hi, NULL);
    probe("popup", d, 0, 0, 0, 0);

    printf("done\n");
    return 0;
}
