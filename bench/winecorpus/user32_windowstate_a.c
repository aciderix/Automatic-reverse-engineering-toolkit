/* Exercises the USER32 *extended window model* (G2a, doc 72) — window geometry,
 * show/enable state, GWL storage, and window-text round-trip — still display-free
 * (no pixels; SDL_Window is G2b). Everything printed is deterministic and identical
 * under Wine and ARET, so it is checkable bit-for-bit.
 *
 *  - CreateWindowExA at explicit coords -> GetWindowRect returns {x,y,x+w,y+h}.
 *  - SetWindowPos (move-only, then size-only) and MoveWindow update the rect.
 *  - ShowWindow returns the previous visibility; IsWindowVisible tracks it.
 *  - EnableWindow returns the previous *disabled* state; IsWindowEnabled tracks it.
 *  - SetWindowLong/GetWindowLong(GWL_USERDATA) round-trips app storage.
 *  - SetWindowTextA/GetWindowTextA/GetWindowTextLengthA round-trip via WM_SETTEXT/
 *    WM_GETTEXT through the guest WNDPROC -> DefWindowProcA (real Windows path).
 *  - GetParent (top-level -> 0), IsWindow, GetDesktopWindow, GetSystemMetrics are
 *    checked by *invariant* where the raw value is environment-dependent (screen
 *    size), by exact value where it is not.
 *
 * A WS_POPUP window (no caption/border, no window-manager clamping) keeps the
 * geometry deterministic headless. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcA(h, msg, wp, lp);   /* store/report text via WM_*TEXT* */
}

int main(void) {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "AretStateClass";
    printf("register ok=%d\n", RegisterClassA(&wc) != 0);

    HWND hwnd = CreateWindowExA(0, "AretStateClass", "win-a", WS_POPUP,
                                100, 50, 400, 300,
                                NULL, NULL, wc.hInstance, NULL);
    printf("create ok=%d\n", hwnd != NULL);

    RECT r;
    GetWindowRect(hwnd, &r);
    printf("rect0 %ld,%ld,%ld,%ld\n", (long)r.left, (long)r.top, (long)r.right, (long)r.bottom);

    /* Move only (keep size). */
    SetWindowPos(hwnd, NULL, 200, 80, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    GetWindowRect(hwnd, &r);
    printf("rect1 %ld,%ld,%ld,%ld\n", (long)r.left, (long)r.top, (long)r.right, (long)r.bottom);

    /* Size only (keep position). */
    SetWindowPos(hwnd, NULL, 0, 0, 640, 480, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    GetWindowRect(hwnd, &r);
    printf("rect2 %ld,%ld,%ld,%ld\n", (long)r.left, (long)r.top, (long)r.right, (long)r.bottom);

    /* MoveWindow sets both. */
    MoveWindow(hwnd, 10, 20, 300, 200, FALSE);
    GetWindowRect(hwnd, &r);
    printf("rect3 %ld,%ld,%ld,%ld\n", (long)r.left, (long)r.top, (long)r.right, (long)r.bottom);

    /* Show/hide: ShowWindow returns the *previous* visibility. */
    int was1 = ShowWindow(hwnd, SW_SHOW);
    int vis1 = IsWindowVisible(hwnd);
    int was2 = ShowWindow(hwnd, SW_HIDE);
    int vis2 = IsWindowVisible(hwnd);
    printf("show was=%d,%d vis=%d,%d\n", was1, was2, vis1, vis2);

    /* Enable/disable: EnableWindow returns the *previous disabled* state. */
    int en0 = IsWindowEnabled(hwnd);
    int wd1 = EnableWindow(hwnd, FALSE);   /* was enabled -> previously-disabled = 0 */
    int en1 = IsWindowEnabled(hwnd);
    int wd2 = EnableWindow(hwnd, TRUE);    /* was disabled -> previously-disabled = 1 */
    int en2 = IsWindowEnabled(hwnd);
    printf("enable start=%d wd=%d,%d en=%d,%d\n", en0, wd1, wd2, en1, en2);

    /* GWL_USERDATA round-trip (app storage). */
    LONG prev = SetWindowLongA(hwnd, GWL_USERDATA, 0x12345678);
    LONG got  = GetWindowLongA(hwnd, GWL_USERDATA);
    printf("userdata prev=%ld got=0x%lx\n", (long)prev, (unsigned long)got);

    /* Window-text round-trip via WM_SETTEXT/WM_GETTEXT -> DefWindowProcA. */
    SetWindowTextA(hwnd, "Hello ARET");
    char buf[64];
    int n = GetWindowTextA(hwnd, buf, sizeof buf);
    int len = GetWindowTextLengthA(hwnd);
    printf("text n=%d len=%d val=[%s]\n", n, len, buf);

    /* Relations / handles. */
    printf("parent=%d iswin=%d isbogus=%d desktop_ok=%d\n",
           (int)(GetParent(hwnd) == NULL), IsWindow(hwnd) != 0,
           IsWindow((HWND)0x7ffffff0) != 0, GetDesktopWindow() != NULL);

    /* Screen metrics are environment-dependent: check the invariant, not the raw
     * value (identical true/true under Wine-Xvfb and ARET-headless). */
    printf("metrics ok=%d\n", GetSystemMetrics(SM_CXSCREEN) > 0 && GetSystemMetrics(SM_CYSCREEN) > 0);

    DestroyWindow(hwnd);
    UnregisterClassA("AretStateClass", wc.hInstance);
    printf("done\n");
    return 0;
}
