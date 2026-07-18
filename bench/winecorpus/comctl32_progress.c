/* A real stateful comctl32 CONTROL (progress bar), lifted from Wine's comctl32:
   the class is registered by the lifted DllMain, the control window is created
   (WM_NCCREATE/WM_CREATE allocate its state into cbWndExtra), and PBM_* messages
   dispatch to the lifted comctl32 WNDPROC — its range/pos state round-trips
   bit-identically to Wine. OpenThemeData is delay-loaded (uxtheme) and resolved
   to "no theme" so the control takes its classic path. Message-only window keeps
   it display-free. */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
int main(void) {
    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&ic);
    HWND pb = CreateWindowExA(0, PROGRESS_CLASSA, NULL, WS_CHILD, 0, 0, 100, 20,
                              HWND_MESSAGE, NULL, GetModuleHandleA(NULL), NULL);
    printf("pb=%d\n", pb != NULL);
    SendMessageA(pb, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageA(pb, PBM_SETPOS, 50, 0);
    printf("pos=%d lo=%d hi=%d\n", (int)SendMessageA(pb, PBM_GETPOS, 0, 0),
           (int)SendMessageA(pb, PBM_GETRANGE, TRUE, 0),
           (int)SendMessageA(pb, PBM_GETRANGE, FALSE, 0));
    SendMessageA(pb, PBM_SETPOS, 75, 0);
    SendMessageA(pb, PBM_DELTAPOS, 10, 0);
    printf("pos2=%d\n", (int)SendMessageA(pb, PBM_GETPOS, 0, 0));
    return 0;
}
