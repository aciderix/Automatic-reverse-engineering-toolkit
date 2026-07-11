/* G2b — the modern window-class path: RegisterClassExA (WNDCLASSEX, the form
 * virtually every real Win32 GUI app uses) with a class background brush, then a
 * visible window that paints. Exercises the WNDCLASSEX field offsets (+8 wndproc,
 * +32 hbrBackground, +40 className) plus the whole show/erase/paint chain, all
 * bit-identical to Wine headless. WS_POPUP => client == window. */
#include <windows.h>
#include <stdio.h>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        printf("bg=%06lX\n", (unsigned long)GetPixel(dc, 4, 4));  /* class brush */
        SetPixel(dc, 2, 2, RGB(90, 180, 240));
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

int main(void) {
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = "AretRcex";
    wc.hbrBackground = CreateSolidBrush(RGB(15, 30, 45));
    ATOM a = RegisterClassExA(&wc);
    printf("atom=%d\n", a ? 1 : 0);

    HWND hw = CreateWindowExA(0, "AretRcex", "x", WS_POPUP | WS_VISIBLE,
                              0, 0, 16, 12, NULL, NULL, NULL, NULL);
    printf("win=%d\n", hw ? 1 : 0);
    UpdateWindow(hw);

    HDC dc = GetDC(hw);
    printf("readback bg=%06lX fg=%06lX\n",
           (unsigned long)GetPixel(dc, 4, 4),
           (unsigned long)GetPixel(dc, 2, 2));
    ReleaseDC(hw, dc);

    DestroyWindow(hw);
    printf("done\n");
    return 0;
}
