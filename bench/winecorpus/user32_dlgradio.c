/* Radio group WITHOUT WS_TABSTOP (like FishTank's), to measure two things vs Wine:
   (1) does the dialog manager report WS_TABSTOP on the radios (GetWindowLong GWL_STYLE)?
   (2) does an MFC-DDX_Radio-style walk (iterate the group via GW_HWNDNEXT, gate BM_SETCHECK
       on WS_TABSTOP, stop at the next WS_GROUP) actually select a radio?
   The .rc uses CONTROL (not AUTORADIOBUTTON, which would add WS_TABSTOP) so the template
   truly lacks it. Deterministic message round-trip; the DLGPROC EndDialogs at once. */
#include <windows.h>
#include <stdio.h>

#define IDD  101
#define IDR0 201
#define IDR1 202
#define IDR2 203

static INT_PTR CALLBACK DlgProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    (void)wp; (void)lp;
    if (m == WM_INITDIALOG) {
        HWND r[3] = { GetDlgItem(h, IDR0), GetDlgItem(h, IDR1), GetDlgItem(h, IDR2) };
        LONG s0 = GetWindowLongA(r[0], GWL_STYLE), s1 = GetWindowLongA(r[1], GWL_STYLE);
        printf("tabstop r0=%d r1=%d  group r0=%d r1=%d\n",
               !!(s0 & WS_TABSTOP), !!(s1 & WS_TABSTOP), !!(s0 & WS_GROUP), !!(s1 & WS_GROUP));
        /* DDX_Radio-style walk: from the group start, select button index 1. */
        int value = 1, iButton = 0;
        HWND c = r[0];
        do {
            if (GetWindowLongA(c, GWL_STYLE) & WS_TABSTOP) {
                SendMessageA(c, BM_SETCHECK, (iButton == value), 0);
                iButton++;
            }
            c = GetWindow(c, GW_HWNDNEXT);
        } while (c != NULL && !(GetWindowLongA(c, GWL_STYLE) & WS_GROUP));
        printf("walk iButtons=%d checks r0=%d r1=%d r2=%d\n", iButton,
               (int)SendMessageA(r[0], BM_GETCHECK, 0, 0),
               (int)SendMessageA(r[1], BM_GETCHECK, 0, 0),
               (int)SendMessageA(r[2], BM_GETCHECK, 0, 0));
        EndDialog(h, 0);
        return TRUE;
    }
    return FALSE;
}

int main(void) {
    DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD), NULL, DlgProc, 0);
    printf("done\n");
    return 0;
}
