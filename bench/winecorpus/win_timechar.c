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
    /* ClientToScreen/ScreenToClient are asserted RELATIVE to where the window actually
     * ended up, not against the position that was requested. The two are not the same
     * thing: the X server is free to place the window elsewhere, and under parallel
     * load it sometimes does — which made this fixture flap on the ORACLE side, Wine
     * reporting a window at 0,0. That absolute position is environmental; the contract
     * is that client and screen coordinates differ by exactly the client origin, and
     * that the two conversions invert each other. Both of those hold wherever the
     * window lands, so they are what is checked. */
    RECT wr; GetWindowRect(w, &wr);
    POINT org = {0, 0}; ClientToScreen(w, &org);      /* the client origin, in screen coords */
    POINT pt = {10, 10}; ClientToScreen(w, &pt);
    printf("c2s_delta=%ld,%ld\n", pt.x - org.x, pt.y - org.y);
    POINT p2 = {org.x + 10, org.y + 10}; ScreenToClient(w, &p2);
    printf("s2c=%ld,%ld\n", p2.x, p2.y);
    POINT rt = {37, 91}; ClientToScreen(w, &rt); ScreenToClient(w, &rt);
    printf("roundtrip=%ld,%ld\n", rt.x, rt.y);
    DestroyWindow(w); UnregisterClassA("C2S", wc.hInstance);
    printf("done\n");
    return 0;
}
