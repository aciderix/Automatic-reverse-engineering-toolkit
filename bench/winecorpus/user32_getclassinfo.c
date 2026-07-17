/* GetClassInfoA / GetClassInfoExA — read a registered window class back. A program
 * that registers a class then queries it must read back exactly what it set; ARET's
 * class registry stores the full WNDCLASS(EX) so GetClassInfo returns it verbatim,
 * like Wine. Handles/pointers (wndproc, hInstance, hIcon, hCursor, hbrBackground) are
 * non-deterministic addresses, so the fixture compares them for round-trip equality
 * rather than printing raw values; the scalar fields (style, cbClsExtra, cbWndExtra)
 * are printed verbatim. An unregistered class must return 0. Expected identical under
 * Wine and ARET. */
#include <windows.h>
#include <stdio.h>
static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }
int main(void) {
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;              /* 0x0003 */
    wc.lpfnWndProc = WP;
    wc.cbClsExtra = 8; wc.cbWndExtra = 16;
    wc.hInstance = GetModuleHandleA(0);
    wc.hIcon = LoadIconA(0, (LPCSTR)0x7F00);
    wc.hCursor = LoadCursorA(0, (LPCSTR)0x7F00);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "MyCls";
    printf("reg_ok=%d\n", RegisterClassA(&wc) != 0);

    WNDCLASSA g; memset(&g, 0xAB, sizeof g);
    int found = GetClassInfoA(GetModuleHandleA(0), "MyCls", &g) != 0;
    printf("found=%d style=%#x clsx=%d wndx=%d wp=%d hbr=%d hinst=%d icon=%d cur=%d\n",
        found, (unsigned)g.style, g.cbClsExtra, g.cbWndExtra,
        g.lpfnWndProc == WP, g.hbrBackground == (HBRUSH)(COLOR_WINDOW + 1),
        g.hInstance == GetModuleHandleA(0), g.hIcon == wc.hIcon, g.hCursor == wc.hCursor);

    WNDCLASSEXA gx; memset(&gx, 0xAB, sizeof gx);
    int foundx = GetClassInfoExA(GetModuleHandleA(0), "MyCls", &gx) != 0;
    printf("foundx=%d cbSize=%d style=%#x clsx=%d wndx=%d wp=%d\n",
        foundx, gx.cbSize, (unsigned)gx.style, gx.cbClsExtra, gx.cbWndExtra, gx.lpfnWndProc == WP);

    WNDCLASSA miss; memset(&miss, 0, sizeof miss);
    printf("missing=%d\n", GetClassInfoA(GetModuleHandleA(0), "NoSuch", &miss) != 0);
    return 0;
}
