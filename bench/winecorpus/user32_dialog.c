/* Exercises the dialog subsystem (G5b, doc 72), display-free model: DialogBoxParamA
 * parses the DLGTEMPLATE, creates child controls (STATIC/EDIT/BUTTON) with their
 * template text + ids, and calls the DLGPROC back (via aret_call) with WM_INITDIALOG.
 * The proc reads a control's template text, round-trips edit text + an int, checks
 * GetDlgItem/GetDlgCtrlID, then EndDialog(77) — so the modal pump returns at once
 * (no user input; deterministic under Xvfb). Everything printed is exact -> checkable
 * bit-for-bit against Wine. */
#include <windows.h>
#include <stdio.h>

#define IDD_TEST   101
#define IDC_LABEL  1001
#define IDC_EDIT   1002
#define IDC_OKBTN  1003

static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    (void)wp; (void)lp;
    if (msg == WM_INITDIALOG) {
        char buf[64];
        int n = GetDlgItemTextA(hDlg, IDC_LABEL, buf, sizeof buf);
        printf("label n=%d [%s]\n", n, buf);

        HWND item = GetDlgItem(hDlg, IDC_OKBTN);
        printf("btn found=%d id=%d\n", item != NULL, GetDlgCtrlID(item));

        SetDlgItemTextA(hDlg, IDC_EDIT, "typed value");
        n = GetDlgItemTextA(hDlg, IDC_EDIT, buf, sizeof buf);
        printf("edit n=%d [%s]\n", n, buf);

        SetDlgItemInt(hDlg, IDC_EDIT, 4242, FALSE);
        BOOL ok = FALSE;
        UINT v = GetDlgItemInt(hDlg, IDC_EDIT, &ok, FALSE);
        printf("int ok=%d v=%u\n", ok, v);

        EndDialog(hDlg, 77);
        return TRUE;
    }
    return FALSE;
}

int main(void) {
    HINSTANCE hInst = GetModuleHandleA(NULL);
    INT_PTR r = DialogBoxParamA(hInst, MAKEINTRESOURCEA(IDD_TEST), NULL, DlgProc, 0);
    printf("dialog ret=%d\n", (int)r);
    printf("done\n");
    return 0;
}
