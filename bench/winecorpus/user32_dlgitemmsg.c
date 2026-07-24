/* Populating predefined controls through SendDlgItemMessage / SendMessage(GetDlgItem)
   — the path a dialog uses to fill a COMBOBOX (CB_ADDSTRING/CB_SETCURSEL) and set a
   BUTTON check (BM_SETCHECK). A system control (no app WNDPROC) must answer BOTH its
   class messages AND the common text messages, whichever entry point addresses it.
   Display-free (never shown): a pure message round-trip, deterministic vs Wine. */
#include <windows.h>
#include <stdio.h>

#define ID_COMBO 1001
#define ID_CHECK 1002
#define ID_EDIT  1003

int main(void) {
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "dlgitemtest";
    RegisterClassA(&wc);
    HWND parent = CreateWindowExA(0, "dlgitemtest", "", WS_OVERLAPPED,
                                  0, 0, 200, 200, NULL, NULL, wc.hInstance, NULL);
    CreateWindowExA(0, "COMBOBOX", NULL, WS_CHILD | CBS_DROPDOWNLIST,
                    0, 0, 120, 100, parent, (HMENU)ID_COMBO, wc.hInstance, NULL);
    CreateWindowExA(0, "BUTTON", "chk", WS_CHILD | BS_AUTOCHECKBOX,
                    0, 0, 120, 20, parent, (HMENU)ID_CHECK, wc.hInstance, NULL);
    HWND edit = CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_BORDER,
                                0, 0, 120, 20, parent, (HMENU)ID_EDIT, wc.hInstance, NULL);

    /* Combo: fill + select via SendDlgItemMessage (the entry point that used to drop
       CB_* on the floor when a control has no app WNDPROC). */
    SendDlgItemMessageA(parent, ID_COMBO, CB_ADDSTRING, 0, (LPARAM)"Alpha");
    SendDlgItemMessageA(parent, ID_COMBO, CB_ADDSTRING, 0, (LPARAM)"Bravo");
    SendDlgItemMessageA(parent, ID_COMBO, CB_ADDSTRING, 0, (LPARAM)"Charlie");
    SendDlgItemMessageA(parent, ID_COMBO, CB_SETCURSEL, 1, 0);
    /* Checkbox: set state via SendDlgItemMessage(BM_SETCHECK). */
    SendDlgItemMessageA(parent, ID_CHECK, BM_SETCHECK, BST_CHECKED, 0);
    /* Edit: set text via SendMessage(GetDlgItem, WM_SETTEXT) — the other entry point. */
    SendMessageA(GetDlgItem(parent, ID_EDIT), WM_SETTEXT, 0, (LPARAM)"hi");

    int count = (int)SendDlgItemMessageA(parent, ID_COMBO, CB_GETCOUNT, 0, 0);
    int sel   = (int)SendDlgItemMessageA(parent, ID_COMBO, CB_GETCURSEL, 0, 0);
    char cbuf[64] = { 0 };
    SendDlgItemMessageA(parent, ID_COMBO, CB_GETLBTEXT, sel, (LPARAM)cbuf);
    int chk   = (int)SendDlgItemMessageA(parent, ID_CHECK, BM_GETCHECK, 0, 0);
    char ebuf[64] = { 0 };
    SendMessageA(edit, WM_GETTEXT, sizeof ebuf, (LPARAM)ebuf);

    printf("count=%d sel=%d text=%s chk=%d edit=%s\n", count, sel, cbuf, chk, ebuf);
    return 0;
}
