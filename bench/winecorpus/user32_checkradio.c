/* CheckRadioButton(hDlg, idFirst, idLast, idCheck) vs Wine, byte-exact.
   Creates a hidden parent + three child BS_RADIOBUTTON controls (ids 201..203), then
   drives CheckRadioButton over three cases and reads each button back with
   IsDlgButtonChecked — proving the group semantics: exactly idCheck ends CHECKED, the
   other in-range buttons UNCHECKED, and buttons outside [idFirst,idLast] are untouched.
   Deterministic: no message loop, no paint, no timing — pure state round-trip. */
#include <windows.h>
#include <stdio.h>

#define IDR0 201
#define IDR1 202
#define IDR2 203

static int chk(HWND p, int id) { return (int)IsDlgButtonChecked(p, id); }

int main(void) {
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "aret_crb_parent";
    RegisterClassA(&wc);
    HWND p = CreateWindowA("aret_crb_parent", "p", WS_OVERLAPPEDWINDOW,
                           0, 0, 200, 200, NULL, NULL, wc.hInstance, NULL);
    HWND r0 = CreateWindowA("BUTTON", "r0", WS_CHILD | BS_RADIOBUTTON, 0,  0, 90, 20, p, (HMENU)IDR0, wc.hInstance, NULL);
    HWND r1 = CreateWindowA("BUTTON", "r1", WS_CHILD | BS_RADIOBUTTON, 0, 20, 90, 20, p, (HMENU)IDR1, wc.hInstance, NULL);
    HWND r2 = CreateWindowA("BUTTON", "r2", WS_CHILD | BS_RADIOBUTTON, 0, 40, 90, 20, p, (HMENU)IDR2, wc.hInstance, NULL);
    printf("created p=%d r0=%d r1=%d r2=%d\n", p != NULL, r0 != NULL, r1 != NULL, r2 != NULL);

    /* Pre-check r0 so a correct CheckRadioButton must UNCHECK it. */
    SendMessageA(r0, BM_SETCHECK, BST_CHECKED, 0);
    printf("before   %d %d %d\n", chk(p, IDR0), chk(p, IDR1), chk(p, IDR2));

    /* Case 1: select the middle button. */
    CheckRadioButton(p, IDR0, IDR2, IDR1);
    printf("check201 %d %d %d\n", chk(p, IDR0), chk(p, IDR1), chk(p, IDR2));

    /* Case 2: move the selection to the last button. */
    CheckRadioButton(p, IDR0, IDR2, IDR2);
    printf("check203 %d %d %d\n", chk(p, IDR0), chk(p, IDR1), chk(p, IDR2));

    /* Case 3: range [IDR0,IDR1] only — r2 must keep its state, r0 becomes checked. */
    CheckRadioButton(p, IDR0, IDR1, IDR0);
    printf("range01  %d %d %d\n", chk(p, IDR0), chk(p, IDR1), chk(p, IDR2));

    DestroyWindow(p);
    printf("done\n");
    return 0;
}
