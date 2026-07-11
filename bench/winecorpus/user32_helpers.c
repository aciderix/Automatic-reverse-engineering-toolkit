/* Exercises the USER32 window-helper batch (long-tail, doc 72 G7), display-free:
 * GetClientRect / AdjustWindowRect / SetForegroundWindow / InvalidateRect /
 * MessageBeep / CallWindowProcA / LoadCursorA / LoadIconA / MsgWaitForMultipleObjects.
 * All values were measured against Wine (Xvfb) and are deterministic. A WS_POPUP
 * window (no non-client area) keeps client/adjust exact. */
#include <windows.h>
#include <stdio.h>

static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

int main(void) {
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WP; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "AretHelpers";
    RegisterClassA(&wc);
    HWND h = CreateWindowExA(0, "AretHelpers", "t", WS_POPUP, 100, 50, 400, 300, NULL, NULL, wc.hInstance, NULL);

    RECT cr; GetClientRect(h, &cr);
    printf("client=%ld,%ld,%ld,%ld\n", cr.left, cr.top, cr.right, cr.bottom);
    RECT ar = {0, 0, 400, 300}; AdjustWindowRect(&ar, WS_POPUP, FALSE);
    printf("adj=%ld,%ld,%ld,%ld\n", ar.left, ar.top, ar.right, ar.bottom);
    printf("setfg=%d inval=%d beep=%d\n",
           SetForegroundWindow(h) != 0, InvalidateRect(h, NULL, TRUE) != 0, MessageBeep(0) != 0);
    printf("callwp=%ld\n", (long)CallWindowProcA((WNDPROC)WP, h, WM_NULL, 0, 0));
    printf("cursor=%d icon=%d\n", LoadCursorA(NULL, IDC_ARROW) != NULL, LoadIconA(NULL, IDI_APPLICATION) != NULL);
    printf("mwfmo=%d\n", MsgWaitForMultipleObjects(0, NULL, FALSE, 0, QS_ALLINPUT));

    DestroyWindow(h); UnregisterClassA("AretHelpers", wc.hInstance);
    printf("done\n");
    return 0;
}
