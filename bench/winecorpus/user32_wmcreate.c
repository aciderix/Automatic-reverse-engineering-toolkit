/* Window creation sends WM_NCCREATE then WM_CREATE to the WNDPROC, and the class's
   cbWndExtra bytes are real storage (SetWindowLong/GetWindowLong at offset >=0) —
   how a control (comctl32) stashes its per-window state pointer. Message-only
   window (HWND_MESSAGE) so it is display-free. Bit-identical to Wine. */
#include <windows.h>
#include <stdio.h>
static int g_created = 0, g_ncc = 0;
LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) g_ncc = 1;
    if (m == WM_CREATE) { g_created = 1; SetWindowLongA(h, 0, 0x1234); SetWindowLongA(h, 4, 0x5678); }
    return DefWindowProcA(h, m, w, l);
}
int main(void) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "T"; wc.cbWndExtra = 8;
    RegisterClassA(&wc);
    HWND h = CreateWindowExA(0, "T", "t", 0, 0, 0, 10, 10, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    printf("ncc=%d created=%d win=%d e0=%lx e4=%lx\n", g_ncc, g_created, h != NULL,
           GetWindowLongA(h, 0), GetWindowLongA(h, 4));
    return 0;
}
