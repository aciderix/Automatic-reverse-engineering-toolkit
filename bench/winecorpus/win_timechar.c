/* Long-tail: FileTimeToSystemTime, CharNextA/CharPrevA, GetDriveTypeA,
 * ClientToScreen/ScreenToClient. Measured vs Wine, deterministic. */
#include <windows.h>
#include <stdio.h>
static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }
int main(void) {
    FILETIME ft; ULONGLONG t = 125911584000000000ULL;   /* 2000-01-01 00:00:00 UTC */
    ft.dwLowDateTime = (DWORD)t; ft.dwHighDateTime = (DWORD)(t >> 32);
    SYSTEMTIME st; FileTimeToSystemTime(&ft, &st);
    printf("fts %d-%02d-%02d %02d:%02d:%02d dow=%d\n",
           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wDayOfWeek);
    const char *s = "abc";
    printf("cn=%d cnend=%d cp=%d drive=%d\n",
           (int)(CharNextA(s) - s), (int)(CharNextA(s + 3) - s), (int)(CharPrevA(s, s + 2) - s), GetDriveTypeA("C:\\"));
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WP; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "C2S";
    RegisterClassA(&wc);
    HWND w = CreateWindowExA(0, "C2S", "t", WS_POPUP, 100, 50, 400, 300, NULL, NULL, wc.hInstance, NULL);
    POINT pt = {10, 10}; ClientToScreen(w, &pt);
    printf("c2s=%ld,%ld\n", pt.x, pt.y);
    POINT p2 = {110, 60}; ScreenToClient(w, &p2);
    printf("s2c=%ld,%ld\n", p2.x, p2.y);
    DestroyWindow(w); UnregisterClassA("C2S", wc.hInstance);
    printf("done\n");
    return 0;
}
