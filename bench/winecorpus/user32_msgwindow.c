/* Exercises the USER32 *message-only window* subsystem (no pixels): register a
 * window class, create a message-only window (HWND_MESSAGE), then drive messages
 * through it both ways — SendMessageW (synchronous, calls the WNDPROC directly)
 * and PostMessageW + GetMessageW + DispatchMessageW (queued, pumped by a loop) —
 * plus PostQuitMessage/WM_QUIT and teardown. The WNDPROC is a callback *back into
 * the lifted program*, so this proves ARET can register a guest callback, queue
 * Win32 messages, and dispatch them to the guest with the right wParam/lParam.
 * Message-only windows never display, so this is fully portable/sound (no X11).
 * Deterministic output → checkable bit-for-bit against Wine. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int   g_received = 0;
static UINT  g_last_msg = 0;
static WPARAM g_wp = 0;
static LPARAM g_lp = 0;

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP + 1) {
        g_received++;
        g_last_msg = msg;
        g_wp = wp;
        g_lp = lp;
        return 4200 + (LRESULT)wp; /* a value the caller of SendMessage can check */
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int main(void) {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"AretMsgClass";

    ATOM atom = RegisterClassW(&wc);
    printf("register ok=%d\n", atom != 0);

    HWND hwnd = CreateWindowExW(0, L"AretMsgClass", L"t", 0,
                                0, 0, 0, 0,
                                HWND_MESSAGE, NULL, wc.hInstance, NULL);
    printf("create ok=%d\n", hwnd != NULL);

    /* SendMessageW: synchronous — must invoke WndProc right now and return its value. */
    LRESULT r = SendMessageW(hwnd, WM_APP + 1, 111, 222);
    printf("send ret=%ld received=%d wp=%ld lp=%ld\n",
           (long)r, g_received, (long)g_wp, (long)g_lp);

    /* PostMessageW + GetMessageW + DispatchMessageW: the queued path. */
    PostMessageW(hwnd, WM_APP + 1, 333, 444);
    MSG m;
    memset(&m, 0, sizeof(m));
    if (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    printf("dispatch received=%d wp=%ld lp=%ld\n", g_received, (long)g_wp, (long)g_lp);

    /* PeekMessageW on an empty queue must report "nothing". */
    int peek = PeekMessageW(&m, NULL, 0, 0, PM_REMOVE);
    printf("peek empty=%d\n", peek);

    /* PostQuitMessage -> GetMessageW returns 0 with WM_QUIT and the exit code. */
    PostQuitMessage(7);
    int gr = GetMessageW(&m, NULL, 0, 0);
    printf("quit getmsg=%d is_wm_quit=%d code=%d\n",
           gr, m.message == WM_QUIT, (int)m.wParam);

    DestroyWindow(hwnd);
    UnregisterClassW(L"AretMsgClass", wc.hInstance);
    printf("done\n");
    return 0;
}
