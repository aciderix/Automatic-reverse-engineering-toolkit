/* DialogBoxIndirectParamA — a modal dialog from an in-memory DLGTEMPLATE (not a
 * resource). A minimal zero-control template is built by hand; the DLGPROC EndDialogs
 * on WM_INITDIALOG with a chosen code, which the modal pump returns. Same machinery as
 * DialogBoxParam, only the template source differs (a direct pointer vs a resource id).
 * Expected identical under Wine and ARET (headless: the DLGPROC ends the loop itself,
 * so no real window events are needed). */
#include <windows.h>
#include <stdio.h>
#pragma pack(push, 1)
typedef struct {
    DLGTEMPLATE dt;      /* style, dwExtendedStyle, cdit, x, y, cx, cy — 18 bytes */
    WORD menu;           /* 0 = no menu   */
    WORD windowClass;    /* 0 = default   */
    WCHAR title[4];      /* "Tst\0"       */
} DLG;
#pragma pack(pop)

static INT_PTR CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INITDIALOG) {
        printf("init param=%ld\n", (long)l);
        EndDialog(h, 4242);
        return TRUE;
    }
    return FALSE;
}
int main(void) {
    DLG d; memset(&d, 0, sizeof d);
    d.dt.style = DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    d.dt.cdit = 0;                       /* no controls */
    d.dt.cx = 80; d.dt.cy = 40;
    d.title[0] = 'T'; d.title[1] = 's'; d.title[2] = 't'; d.title[3] = 0;
    INT_PTR r = DialogBoxIndirectParamA(GetModuleHandleA(0), &d.dt, NULL, Proc, 77);
    printf("result=%ld\n", (long)r);
    return 0;
}
