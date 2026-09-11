/* comctl32 window subclassing (SetWindowSubclass / DefSubclassProc / GetWindowSubclass /
 * RemoveWindowSubclass). A subclass proc intercepts messages ahead of the window's own
 * wndproc; DefSubclassProc passes control down the chain to the next-older proc and finally
 * to the original wndproc. Modern apps rely on this (notepad subclasses its EDIT). We install
 * two stacked subclasses on a message-only window and check: newest runs first; DefSubclassProc
 * chains to the older then the original; GetWindowSubclass returns the ref data; and after
 * RemoveWindowSubclass the message reaches the original wndproc unmodified. Headless (no
 * display): pure message dispatch, identical vs Wine. */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>

#define WM_PROBE (WM_APP + 7)

static LRESULT CALLBACK OrigProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PROBE) return 100;              /* base contribution */
    return DefWindowProcA(h, m, w, l);
}
/* Outer subclass (installed 2nd -> runs first): adds 20 to whatever the rest returns. */
static LRESULT CALLBACK SubA(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    (void)id; (void)ref;
    if (m == WM_PROBE) return 20 + DefSubclassProc(h, m, w, l);
    return DefSubclassProc(h, m, w, l);
}
/* Inner subclass (installed 1st -> runs second): adds 3. */
static LRESULT CALLBACK SubB(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    (void)id; (void)ref;
    if (m == WM_PROBE) return 3 + DefSubclassProc(h, m, w, l);
    return DefSubclassProc(h, m, w, l);
}

int main(void) {
    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ic);
    WNDCLASSA wc = {0}; wc.lpfnWndProc = OrigProc; wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "AretSubclass"; RegisterClassA(&wc);
    HWND h = CreateWindowExA(0, "AretSubclass", "", WS_OVERLAPPED, 0, 0, 10, 10,
                             HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!h) { printf("no window\n"); return 1; }

    printf("base           = %ld\n", (long)SendMessageA(h, WM_PROBE, 0, 0));   /* 100 */
    SetWindowSubclass(h, SubB, 1, 0xB0B0);
    printf("after SubB     = %ld\n", (long)SendMessageA(h, WM_PROBE, 0, 0));   /* 3+100 = 103 */
    SetWindowSubclass(h, SubA, 2, 0xA0A0);
    printf("after SubA     = %ld\n", (long)SendMessageA(h, WM_PROBE, 0, 0));   /* 20+3+100 = 123 */

    DWORD_PTR ref = 0;
    if (GetWindowSubclass(h, SubA, 2, &ref)) printf("SubA ref       = %#lx\n", (unsigned long)ref);   /* 0xa0a0 */
    if (GetWindowSubclass(h, SubB, 1, &ref)) printf("SubB ref       = %#lx\n", (unsigned long)ref);   /* 0xb0b0 */

    RemoveWindowSubclass(h, SubA, 2);
    printf("after -SubA    = %ld\n", (long)SendMessageA(h, WM_PROBE, 0, 0));   /* 3+100 = 103 */
    RemoveWindowSubclass(h, SubB, 1);
    printf("after -SubB    = %ld\n", (long)SendMessageA(h, WM_PROBE, 0, 0));   /* 100 */
    printf("SubA gone      = %d\n", GetWindowSubclass(h, SubA, 2, &ref) ? 1 : 0);  /* 0 */

    DestroyWindow(h);
    printf("done\n");
    return 0;
}
