/* End-to-end GUI stack (M7, doc 72): a real windowed app that paints text.
 * Exercises the WHOLE integrated chain in one program — RegisterClassA →
 * CreateWindowExA(WS_VISIBLE) → message loop → WM_PAINT dispatched to the lifted
 * WndProc → BeginPaint → CreateFontA/SelectObject → TextOutA (FreeType raster into
 * the window's client framebuffer) → EndPaint → GetPixel readback. In ARET the
 * window is a real SDL window; the text is rasterized with FreeType (the same
 * rasterizer Wine uses) → the painted framebuffer is bit-identical to Wine.
 *
 * The oracle is the readback digest (count + bbox of black text pixels in client
 * coordinates) — deterministic and window-placement independent. Needs a display
 * (Xvfb) for both engines, like the other visible-window fixtures. */
#include <windows.h>
#include <stdio.h>

static const char *g_msg = "ARET GUI";

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        HFONT f = CreateFontA(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, "DejaVu Sans");
        HGDIOBJ old = SelectObject(dc, f);
        SetTextColor(dc, RGB(0, 0, 0));
        SetBkMode(dc, TRANSPARENT);
        TextOutA(dc, 4, 4, g_msg, (int)strlen(g_msg));
        SelectObject(dc, old);
        DeleteObject(f);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int main(void) {
    HINSTANCE hi = GetModuleHandleA(NULL);
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = "AretCls";
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    RegisterClassA(&wc);
    HWND h = CreateWindowExA(0, "AretCls", "t", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        0, 0, 160, 60, NULL, NULL, hi, NULL);
    printf("window=%d\n", h != NULL);
    UpdateWindow(h);   /* force a synchronous WM_PAINT → WndProc paints text */

    MSG msg; int pumped = 0;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE) && pumped < 50) {
        TranslateMessage(&msg); DispatchMessageA(&msg); pumped++;
    }

    /* read back the painted client framebuffer: count/bbox the black text pixels */
    HDC dc = GetDC(h);
    int black = 0, minx = 999, miny = 999, maxx = -1, maxy = -1;
    for (int y = 0; y < 40; y++)
        for (int x = 0; x < 150; x++) {
            COLORREF c = GetPixel(dc, x, y);
            if (c != CLR_INVALID && (c & 0xFFFFFF) == 0) {
                black++;
                if (x < minx) minx = x; if (x > maxx) maxx = x;
                if (y < miny) miny = y; if (y > maxy) maxy = y;
            }
        }
    ReleaseDC(h, dc);
    printf("text_pixels=%d bbox=%d,%d,%d,%d\n", black, minx, miny, maxx, maxy);
    DestroyWindow(h);
    printf("done\n");
    return 0;
}
