/* GetClassNameA (stored class name), CreateFontA/CreateFontIndirectA (opaque GDI
 * font handles: non-null, SelectObject round-trip). Measured vs Wine. */
#include <windows.h>
#include <stdio.h>
static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }
int main(void) {
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WP; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "MyClass";
    RegisterClassA(&wc);
    HWND w = CreateWindowExA(0, "MyClass", "title", WS_POPUP, 0, 0, 50, 50, NULL, NULL, wc.hInstance, NULL);
    char cn[64]; int n = GetClassNameA(w, cn, sizeof cn);
    printf("classname n=%d [%s]\n", n, cn);

    HFONT f = CreateFontA(12, 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 0, 0, "Arial");
    printf("font_nonnull=%d\n", f != NULL);
    LOGFONTA lf; memset(&lf, 0, sizeof lf); lf.lfHeight = 14; strcpy(lf.lfFaceName, "Courier");
    HFONT f2 = CreateFontIndirectA(&lf);
    printf("font2_nonnull=%d distinct=%d\n", f2 != NULL, f != f2);
    HDC dc = GetDC(w); HGDIOBJ prev = SelectObject(dc, f); HGDIOBJ back = SelectObject(dc, prev);
    printf("selfont_roundtrip=%d\n", back == f);
    ReleaseDC(w, dc); DeleteObject(f); DeleteObject(f2);

    DestroyWindow(w); UnregisterClassA("MyClass", wc.hInstance);
    printf("done\n");
    return 0;
}
