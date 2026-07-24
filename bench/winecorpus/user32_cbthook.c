/* WH_CBT window hook — the mechanism MFC uses to attach/subclass its CWnd objects.
   A CBT hook installed before CreateWindow gets HCBT_CREATEWND for the new window;
   the hook subclasses it (SetWindowLong GWL_WNDPROC), so the subclass proc then
   receives the window's messages (exactly AfxHookWindowCreateFilter). Proves the
   hook fires and the subclass takes effect. Needs a real window -> runs under the
   harness Xvfb, but the counts are a deterministic message round-trip, not pixels. */
#include <windows.h>
#include <string.h>
#include <stdio.h>

static HHOOK   g_hook;
static WNDPROC g_orig;
static int     g_my_hooked = 0;   /* HCBT_CREATEWND seen for OUR class (env-independent) */
static int     g_sub_msgs = 0;    /* messages the installed subclass received */

static LRESULT CALLBACK SubProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    g_sub_msgs++;
    return CallWindowProcA(g_orig, h, m, w, l);
}
static LRESULT CALLBACK CbtProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HCBT_CREATEWND) {
        /* Read the class from the CBT_CREATEWND->CREATESTRUCT (what MFC inspects to
           decide whether to attach), and only act on OUR window — Wine also fires this
           for its auto-created IME window, which we must not count. */
        CBT_CREATEWNDA *cc = (CBT_CREATEWNDA *)lParam;
        const char *cls = (cc && cc->lpcs) ? cc->lpcs->lpszClass : NULL;
        if (cls && (ULONG_PTR)cls > 0xFFFF && strcmp(cls, "cbttest") == 0) {
            g_my_hooked = 1;
            g_orig = (WNDPROC)(LONG_PTR)SetWindowLongA((HWND)wParam, GWL_WNDPROC, (LONG)(LONG_PTR)SubProc);
        }
    }
    return CallNextHookEx(g_hook, code, wParam, lParam);
}
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcA(h, m, w, l);
}

int main(void) {
    HINSTANCE hi = GetModuleHandleA(NULL);
    g_hook = SetWindowsHookExA(WH_CBT, CbtProc, NULL, GetCurrentThreadId());
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = "cbttest";
    RegisterClassA(&wc);
    HWND w = CreateWindowExA(0, "cbttest", "", WS_OVERLAPPED, 0, 0, 100, 100,
                             NULL, NULL, hi, NULL);
    SendMessageA(w, WM_USER + 1, 0, 0);   /* routes to the subclass installed by the hook */
    UnhookWindowsHookEx(g_hook);
    printf("hooked=%d sub_msgs_positive=%d win=%d\n",
           g_my_hooked, g_sub_msgs > 0, w != NULL);
    return 0;
}
