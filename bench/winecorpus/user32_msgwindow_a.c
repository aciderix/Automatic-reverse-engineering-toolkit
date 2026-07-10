/* ANSI (A) twin of user32_msgwindow.c — the same message-only window round-trip
 * through the *A* APIs (RegisterClassA / CreateWindowExA / DefWindowProcA / Send /
 * Post / Get / Dispatch / PeekMessageA), which the Win95 corpus uses (those apps
 * are ANSI). Message-only windows have no pixels, so this stays portable/sound.
 * Deterministic output -> checkable bit-for-bit vs Wine. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_received = 0;
static WPARAM g_wp = 0;
static LPARAM g_lp = 0;

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP + 2) {
        g_received++; g_wp = wp; g_lp = lp;
        return 8400 + (LRESULT)wp;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int main(void) {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "AretMsgClassA";

    ATOM atom = RegisterClassA(&wc);
    printf("register ok=%d\n", atom != 0);

    HWND hwnd = CreateWindowExA(0, "AretMsgClassA", "t", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, NULL, wc.hInstance, NULL);
    printf("create ok=%d\n", hwnd != NULL);

    LRESULT r = SendMessageA(hwnd, WM_APP + 2, 55, 66);
    printf("send ret=%ld received=%d wp=%ld lp=%ld\n",
           (long)r, g_received, (long)g_wp, (long)g_lp);

    PostMessageA(hwnd, WM_APP + 2, 77, 88);
    MSG m; memset(&m, 0, sizeof(m));
    if (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    printf("dispatch received=%d wp=%ld lp=%ld\n", g_received, (long)g_wp, (long)g_lp);

    int peek = PeekMessageA(&m, NULL, 0, 0, PM_REMOVE);
    printf("peek empty=%d\n", peek);

    PostQuitMessage(9);
    int gr = GetMessageA(&m, NULL, 0, 0);
    printf("quit getmsg=%d is_wm_quit=%d code=%d\n", gr, m.message == WM_QUIT, (int)m.wParam);

    DestroyWindow(hwnd);
    UnregisterClassA("AretMsgClassA", wc.hInstance);
    printf("done\n");
    return 0;
}
