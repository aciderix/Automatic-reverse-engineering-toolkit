/* Misc long-tail: Global/Local lock+realloc (GMEM_FIXED handle = pointer),
 * GetObjectA on a DIB (BITMAP dims), FindWindowA by title. All measured vs Wine. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

int main(void) {
    HGLOBAL g = GlobalAlloc(GMEM_FIXED, 32);
    char *p = (char *)GlobalLock(g);
    printf("glock_eq=%d unlock=%d\n", p == (char *)g, GlobalUnlock(g) != 0);
    strcpy(p, "hi");
    g = GlobalReAlloc(g, 64, GMEM_MOVEABLE);
    printf("realloc keep=%d\n", strcmp((char *)g, "hi") == 0);
    GlobalFree(g);

    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = 8; bi.bmiHeader.biHeight = -4;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits; HBITMAP hb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    BITMAP bm; int r = GetObjectA(hb, sizeof bm, &bm);
    printf("getobj r=%d w=%ld h=%ld planes=%d bpp=%d wb=%ld\n",
           r, bm.bmWidth, bm.bmHeight, bm.bmPlanes, bm.bmBitsPixel, bm.bmWidthBytes);
    DeleteObject(hb); DeleteDC(dc);

    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WP; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "FWClass";
    RegisterClassA(&wc);
    HWND w = CreateWindowExA(0, "FWClass", "FindMe", WS_POPUP, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
    printf("find=%d findnope=%d\n", FindWindowA(NULL, "FindMe") == w, FindWindowA(NULL, "Nope") == NULL);
    DestroyWindow(w); UnregisterClassA("FWClass", wc.hInstance);
    printf("done\n");
    return 0;
}
