/* GetProp/SetProp/RemoveProp accept EITHER a string pointer OR a MAKEINTATOM integer atom
 * (value < 0x10000). comctl32 keys all its per-window state on a global atom
 * (GlobalAddAtom then SetPropW/GetPropW(hwnd, MAKEINTATOM(atom))), so the prop APIs must key
 * on the atom, not dereference it as a string (that crashes). A prop stored under a global
 * atom is also retrievable under that atom's string name and vice-versa. Headless. */
#include <windows.h>
#include <stdio.h>

int main(void) {
    WNDCLASSW wc = {0}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(0);
    wc.lpszClassName = L"AretPropAtom"; RegisterClassW(&wc);
    HWND h = CreateWindowExW(0, L"AretPropAtom", L"", WS_OVERLAPPED, 0, 0, 10, 10,
                             HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!h) { printf("no window\n"); return 1; }

    ATOM a = GlobalAddAtomW(L"AretPropName");
    LPCWSTR atomkey = (LPCWSTR)(ULONG_PTR)a;               /* MAKEINTATOM(a) */

    /* set by atom, read by atom */
    SetPropW(h, atomkey, (HANDLE)(ULONG_PTR)0x1234);
    printf("get by atom   = %#lx\n", (unsigned long)(ULONG_PTR)GetPropW(h, atomkey));   /* 0x1234 */
    /* the same prop is visible under the atom's NAME */
    printf("get by name   = %#lx\n", (unsigned long)(ULONG_PTR)GetPropW(h, L"AretPropName")); /* 0x1234 */

    /* set by name, read by atom (equivalence the other way) */
    SetPropW(h, L"AretPropName", (HANDLE)(ULONG_PTR)0xBEEF);
    printf("after set name= %#lx\n", (unsigned long)(ULONG_PTR)GetPropW(h, atomkey));   /* 0xbeef */

    /* a different, unset atom is absent */
    ATOM b = GlobalAddAtomW(L"AretOther");
    printf("other absent  = %#lx\n", (unsigned long)(ULONG_PTR)GetPropW(h, (LPCWSTR)(ULONG_PTR)b)); /* 0 */

    /* remove by atom */
    RemovePropW(h, atomkey);
    printf("after remove  = %#lx\n", (unsigned long)(ULONG_PTR)GetPropW(h, atomkey));   /* 0 */

    GlobalDeleteAtom(a); GlobalDeleteAtom(b);
    DestroyWindow(h);
    printf("done\n");
    return 0;
}
