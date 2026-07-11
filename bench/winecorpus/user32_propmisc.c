/* M7 G7 long-tail batch: window property list (SetProp/GetProp/RemoveProp),
 * GlobalHandle (fixed-heap identity), and the win.ini profile reads
 * (GetProfileString/Int) — all display-free and bit-identical to Wine. Volume
 * info / common-file-dialog / WinExec are sound (invariant / cancelled / boundary
 * failure) and not bit-compared. */
#include <windows.h>
#include <stdio.h>
static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProc(h, m, w, l); }
int main(void) {
    WNDCLASSA wc; memset(&wc, 0, sizeof wc); wc.lpfnWndProc = WP; wc.lpszClassName = "PM"; RegisterClassA(&wc);
    HWND w = CreateWindowExA(0, "PM", "p", WS_POPUP, 0, 0, 50, 40, NULL, NULL, NULL, NULL);
    /* property list round-trip */
    SetPropA(w, "AretKey", (HANDLE)0x1234);
    printf("prop_get=%lx\n", (unsigned long)(uintptr_t)GetPropA(w, "AretKey"));
    printf("prop_miss=%lx\n", (unsigned long)(uintptr_t)GetPropA(w, "Nope"));
    printf("prop_rm=%lx\n", (unsigned long)(uintptr_t)RemovePropA(w, "AretKey"));
    printf("prop_after=%lx\n", (unsigned long)(uintptr_t)GetPropA(w, "AretKey"));
    /* GlobalHandle: fixed heap -> handle == pointer */
    void *p = GlobalAlloc(GMEM_FIXED, 32);
    printf("gh_id=%d\n", GlobalHandle(p) == (HGLOBAL)p);
    GlobalFree(p);
    /* win.ini profile reads: a unique missing key returns the default */
    char buf[64]; GetProfileStringA("AretSect", "AretMiss", "DEFVAL", buf, sizeof buf);
    printf("profile_str=%s\n", buf);
    printf("profile_int=%u\n", GetProfileIntA("AretSect", "AretMiss", 4242));
    DestroyWindow(w);
    printf("done\n");
    return 0;
}
