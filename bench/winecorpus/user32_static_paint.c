/* STATIC control paints: SS_LEFT label fills COLOR_3DFACE (dialog bg, measured vs Wine)
 * then draws its text left/top in COLOR_WINDOWTEXT. Verified via WM_PRINTCLIENT into a
 * memory DIB, structurally (index-based, theme-independent: pixel == GetSysColor(idx)
 * prints the same 1 on ARET-classic and Wine-modern); text pixels are font-dependent so
 * only their existence is asserted (gdi_uifont env caveat). Needs a display (control). */
#include <windows.h>
#include <stdio.h>
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProc(h, m, w, l); }
int main(void) {
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "SPar";
    RegisterClassA(&wc);
    HWND par = CreateWindowExA(0, "SPar", "p", WS_OVERLAPPEDWINDOW, 0, 0, 200, 200, NULL, NULL, wc.hInstance, NULL);
    const int W = 80, H = 20;
    HWND st = CreateWindowExA(0, "STATIC", "Label", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, W, H, par, (HMENU)1, wc.hInstance, NULL);
    HFONT f = CreateFontA(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, 0, 0, "DejaVu Sans");
    SendMessageA(st, WM_SETFONT, (WPARAM)f, 0);
    HDC md = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *b = NULL; HBITMAP hb = CreateDIBSection(md, &bi, DIB_RGB_COLORS, &b, NULL, 0); SelectObject(md, hb);
    unsigned *px = (unsigned *)b; for (int i = 0; i < W * H; i++) px[i] = 0x00FF00FF;
    SendMessageA(st, WM_PRINTCLIENT, (WPARAM)md, PRF_CLIENT);
#define AT(x, y) (px[(y) * W + (x)] & 0xFFFFFF)
#define SC(i)    (GetSysColor(i) & 0xFFFFFF)
    int textpix = 0, magenta = 0;
    for (int i = 0; i < W * H; i++) { unsigned c = px[i] & 0xFFFFFF;
        if (c == 0xFF00FF) magenta++; else if (c != SC(COLOR_3DFACE)) textpix++; }
    printf("bg=%d magenta=%d text_drawn=%d\n", AT(0, 0) == SC(COLOR_3DFACE), magenta, textpix > 0);
    printf("done\n");
    return 0;
}
