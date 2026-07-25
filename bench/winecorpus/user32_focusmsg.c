/* SetFocus must send WM_KILLFOCUS to the window losing focus and WM_SETFOCUS to the
   window gaining it, with the right wParam (the other window) and order. HWND values are
   non-deterministic, so the log names windows relatively (w1/w2/0). Measured vs Wine so
   our aret_SetFocus matches. Runs under the harness Xvfb (real top-level windows). */
#include <windows.h>
#include <stdio.h>

static HWND g_w1, g_w2;
static char g_log[512];
static int  g_n;

static const char *nm(HWND h) { return h == g_w1 ? "w1" : h == g_w2 ? "w2" : h == NULL ? "0" : "?"; }

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SETFOCUS)
        g_n += snprintf(g_log + g_n, sizeof g_log - g_n, "%s:SET(from %s) ", nm(h), nm((HWND)w));
    else if (m == WM_KILLFOCUS)
        g_n += snprintf(g_log + g_n, sizeof g_log - g_n, "%s:KILL(to %s) ", nm(h), nm((HWND)w));
    return DefWindowProcA(h, m, w, l);
}

int main(void) {
    HINSTANCE hi = GetModuleHandleA(NULL);
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = "fmw";
    RegisterClassA(&wc);
    g_w1 = CreateWindowExA(0, "fmw", "1", WS_OVERLAPPEDWINDOW, 0, 0, 80, 60, NULL, NULL, hi, NULL);
    g_w2 = CreateWindowExA(0, "fmw", "2", WS_OVERLAPPEDWINDOW, 90, 0, 80, 60, NULL, NULL, hi, NULL);
    ShowWindow(g_w1, SW_SHOW); ShowWindow(g_w2, SW_SHOW);
    SetFocus(g_w2);   /* establish a known starting focus before the measured sequence */
    /* Reset the log after settle, then drive focus explicitly. GetFocus (thread-local)
       and the WM_SETFOCUS/WM_KILLFOCUS sequence are deterministic; the SetFocus return
       value depends on window activation (no WM under Xvfb) so it is not asserted. */
    g_n = 0; g_log[0] = 0;
    SetFocus(g_w1);
    const char *f1 = nm(GetFocus());
    SetFocus(g_w2);
    const char *f2 = nm(GetFocus());
    printf("after1=%s after2=%s\nseq: %s\n", f1, f2, g_log);
    return 0;
}
