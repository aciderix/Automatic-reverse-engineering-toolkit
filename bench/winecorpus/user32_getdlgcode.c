/* WM_GETDLGCODE per predefined control class — the dialog manager and MFC (DDX_Radio,
   IsDialogMessage) query it to classify a control (DLGC_RADIOBUTTON / DLGC_BUTTON /
   DLGC_STATIC / edit key wants). Returns are measured against Wine so our u32_control_proc
   can match bit-for-bit. Display-free-ish message round-trip under the harness Xvfb. */
#include <windows.h>
#include <stdio.h>

static HWND mk(HWND p, const char *cls, DWORD style, int id) {
    return CreateWindowExA(0, cls, "x", WS_CHILD | style, 0, 0, 60, 16,
                           p, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
}
int main(void) {
    HINSTANCE hi = GetModuleHandleA(NULL);
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = DefWindowProcA; wc.hInstance = hi; wc.lpszClassName = "gdc";
    RegisterClassA(&wc);
    HWND p = CreateWindowExA(0, "gdc", "", WS_OVERLAPPED, 0, 0, 200, 200, NULL, NULL, hi, NULL);
    HWND radio = mk(p, "BUTTON", BS_AUTORADIOBUTTON, 1);
    HWND check = mk(p, "BUTTON", BS_AUTOCHECKBOX, 2);
    HWND push  = mk(p, "BUTTON", BS_PUSHBUTTON, 3);
    HWND defp  = mk(p, "BUTTON", BS_DEFPUSHBUTTON, 4);
    HWND grp   = mk(p, "BUTTON", BS_GROUPBOX, 5);
    HWND edit  = mk(p, "EDIT", 0, 6);
    HWND stat  = mk(p, "STATIC", 0, 7);
    HWND combo = mk(p, "COMBOBOX", CBS_DROPDOWNLIST, 8);
    HWND list  = mk(p, "LISTBOX", 0, 9);
    printf("radio=0x%x check=0x%x push=0x%x defp=0x%x grp=0x%x\n",
           (unsigned)SendMessageA(radio, WM_GETDLGCODE, 0, 0),
           (unsigned)SendMessageA(check, WM_GETDLGCODE, 0, 0),
           (unsigned)SendMessageA(push,  WM_GETDLGCODE, 0, 0),
           (unsigned)SendMessageA(defp,  WM_GETDLGCODE, 0, 0),
           (unsigned)SendMessageA(grp,   WM_GETDLGCODE, 0, 0));
    printf("edit=0x%x static=0x%x combo=0x%x list=0x%x\n",
           (unsigned)SendMessageA(edit,  WM_GETDLGCODE, 0, 0),
           (unsigned)SendMessageA(stat,  WM_GETDLGCODE, 0, 0),
           (unsigned)SendMessageA(combo, WM_GETDLGCODE, 0, 0),
           (unsigned)SendMessageA(list,  WM_GETDLGCODE, 0, 0));
    return 0;
}
