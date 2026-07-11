/* G2b — WM_ERASEBKGND / class background brush. A window class carries an
 * hbrBackground; when the window is (re)painted, BeginPaint sends WM_ERASEBKGND
 * and DefWindowProc fills the client with that brush before the app paints. This
 * is why a real window shows its intended background instead of black. Verified
 * by reading back a pixel the app never drew (must be the class brush colour) and
 * one it did — both bit-identical to Wine, headless. WS_POPUP => client==window. */
#include <windows.h>
#include <stdio.h>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);              /* WM_ERASEBKGND fires here */
        printf("bg_at_paint=%06lX\n", (unsigned long)GetPixel(dc, 5, 5));
        SetPixel(dc, 1, 1, RGB(200, 50, 25));
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

int main(void) {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = "AretErase";
    wc.hbrBackground = CreateSolidBrush(RGB(3, 150, 200));
    RegisterClassA(&wc);

    HWND hw = CreateWindowExA(0, "AretErase", "e", WS_POPUP | WS_VISIBLE,
                              0, 0, 16, 12, NULL, NULL, NULL, NULL);
    UpdateWindow(hw);

    HDC dc = GetDC(hw);
    printf("bg=%06lX fg=%06lX\n",
           (unsigned long)GetPixel(dc, 5, 5),      /* class-brush background */
           (unsigned long)GetPixel(dc, 1, 1));     /* app-drawn pixel */
    ReleaseDC(hw, dc);

    DestroyWindow(hw);
    printf("done\n");
    return 0;
}
