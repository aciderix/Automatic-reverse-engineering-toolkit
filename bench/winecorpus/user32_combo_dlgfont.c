/* Reproduces PuTTY's config-box pattern end-to-end, proving two general user32 behaviours
 * against Wine:
 *
 *  1. WM_GETFONT on a dialog reports the dialog's DS_SETFONT font. A DLGPROC that returns
 *     FALSE leaves the query to the dialog manager (DefDlgProc); PuTTY's ctlposinit does
 *     cp->font = SendMessage(hDlg, WM_GETFONT, 0, 0) and applies that font to controls it
 *     creates dynamically. We build a DS_SETFONT dialog, query WM_GETFONT and print the
 *     returned font's LOGFONT (height + face) via GetObject.
 *
 *  2. A COMBOBOX window sizes to its CLOSED (text-area) height, NOT the cy passed to
 *     CreateWindow (which is only the dropped-down list height). We create a CBS_DROPDOWNLIST
 *     combo with a large cy, apply the dialog font (exactly as PuTTY does), and print its
 *     GetWindowRect/GetClientRect height. It must equal the closed height (tmHeight + 8),
 *     independent of the cy passed.
 *
 * DejaVu Sans is used for DS_SETFONT because both engines resolve it identically (same as
 * user32_dlgcontrols), so the LOGFONT and the font-derived closed height are bit-exact vs
 * Wine. The template is built byte-by-byte with correct DWORD alignment. */
#include <windows.h>
#include <stdio.h>

static BYTE tpl[512];
static int off;
static void al4(void)   { while (off & 3) tpl[off++] = 0; }
static void w16(WORD v) { tpl[off++] = (BYTE)v; tpl[off++] = (BYTE)(v >> 8); }
static void w32(DWORD v){ w16((WORD)v); w16((WORD)(v >> 16)); }
static void wstr(const char *s) { while (*s) w16((WORD)(BYTE)*s++); w16(0); }

static INT_PTR CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)h; (void)m; (void)w; (void)l; return FALSE;   /* everything to DefDlgProc */
}

int main(void) {
    /* DLGTEMPLATE: DS_SETFONT dialog, no controls (added dynamically below, as PuTTY does). */
    w32(DS_SETFONT | WS_POPUP | WS_CAPTION);   /* style   */
    w32(0);                                    /* exStyle */
    w16(0);                                    /* cdit = 0 controls */
    w16(0); w16(0); w16(200); w16(120);        /* x,y,cx,cy (dialog units) */
    w16(0);                                    /* menu    */
    w16(0);                                    /* class   */
    wstr("Dlg");                               /* caption */
    w16(8); wstr("DejaVu Sans");               /* DS_SETFONT: 8pt + typeface */

    HWND h = CreateDialogIndirectParamA(GetModuleHandleA(0), (LPCDLGTEMPLATE)tpl, NULL, Proc, 0);
    if (!h) { printf("no dialog\n"); return 1; }

    /* (1) The dialog font, as PuTTY's ctlposinit reads it. */
    HFONT hf = (HFONT)SendMessageA(h, WM_GETFONT, 0, 0);
    LOGFONTA lf; ZeroMemory(&lf, sizeof lf);
    if (hf && GetObjectA(hf, sizeof lf, &lf))
        printf("dlgfont height=%ld face=%s\n", (long)lf.lfHeight, lf.lfFaceName);
    else
        printf("dlgfont NULL\n");

    /* (2) A combo created with a large dropped cy, then given the dialog font — the combo's
     * window must collapse to the closed field height. */
    HWND cb = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | 0x0003 /*CBS_DROPDOWNLIST*/,
                              10, 20, 120, 100, h, (HMENU)200, GetModuleHandleA(0), NULL);
    if (!cb) { printf("no combo\n"); DestroyWindow(h); return 1; }
    SendMessageA(cb, WM_SETFONT, (WPARAM)hf, MAKELPARAM(TRUE, 0));
    RECT wr; GetWindowRect(cb, &wr);
    RECT cr; GetClientRect(cb, &cr);
    printf("combo win h=%ld client h=%ld (cy passed=100)\n",
           (long)(wr.bottom - wr.top), (long)(cr.bottom - cr.top));

    DestroyWindow(h);
    printf("done\n");
    return 0;
}
