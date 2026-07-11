/* G2b — double-buffered painting, the dominant real-app rendering idiom: on
 * WM_PAINT, build an offscreen 32bpp DIB in a memory DC, draw into it, then
 * BitBlt it to the window DC (whose surface is the window's client framebuffer).
 * Reading the window back afterwards must match Wine bit-for-bit — this exercises
 * the whole chain (window framebuffer binding + CreateDIBSection + FillRect +
 * SetPixel + BitBlt SRCCOPY + present) composing together, headless. */
#include <windows.h>
#include <stdio.h>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HDC mem = CreateCompatibleDC(dc);
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = 16;
        bi.bmiHeader.biHeight = -12;              /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = NULL;
        HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        HGDIOBJ old = SelectObject(mem, bm);
        RECT r = {0, 0, 16, 12};
        HBRUSH br = CreateSolidBrush(RGB(7, 8, 9));
        FillRect(mem, &r, br);
        SetPixel(mem, 3, 2, RGB(240, 130, 60));
        BitBlt(dc, 0, 0, 16, 12, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bm);
        DeleteObject(br);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

int main(void) {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = "AretDbuf";
    RegisterClassA(&wc);

    HWND hw = CreateWindowExA(0, "AretDbuf", "d", WS_POPUP | WS_VISIBLE,
                              0, 0, 16, 12, NULL, NULL, NULL, NULL);
    UpdateWindow(hw);                              /* forces the WM_PAINT that blits */

    HDC dc = GetDC(hw);
    printf("bg=%06lX fg=%06lX\n",
           (unsigned long)GetPixel(dc, 0, 0),
           (unsigned long)GetPixel(dc, 3, 2));
    ReleaseDC(hw, dc);

    DestroyWindow(hw);
    printf("done\n");
    return 0;
}
