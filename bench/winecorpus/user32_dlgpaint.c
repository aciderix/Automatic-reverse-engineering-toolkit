/* Exercises the dialog COMPOSITE path (background fill + child-control paint into the
 * dialog framebuffer) without crashing. The composited pixels can't be diffed against
 * Wine (Wine offers no API to capture its own composited dialog — WM_PRINT PRF_CHILDREN
 * doesn't paint children; the on-screen result is verified qualitatively by an Xvfb
 * screenshot, doc 70 §7). So this is a deterministic non-crash / non-abort guard: create
 * a visible DS_SETFONT dialog with a button (triggering the composite), force a paint,
 * tear it down, and print a fixed line — identical on both engines. Windowed (needs the
 * Xvfb display, like the other visible-window fixtures). */
#include <windows.h>
#include <stdio.h>
static BYTE tpl[256]; static int off;
static void al4(void){ while(off&3) tpl[off++]=0; }
static void w16(WORD v){ tpl[off++]=(BYTE)v; tpl[off++]=(BYTE)(v>>8); }
static void w32(DWORD v){ w16((WORD)v); w16((WORD)(v>>16)); }
static void wstr(const char*s){ while(*s) w16((WORD)(BYTE)*s++); w16(0); }
static INT_PTR CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l){(void)h;(void)m;(void)w;(void)l;return FALSE;}
int main(void){
    w32(DS_SETFONT|WS_VISIBLE|WS_POPUP|WS_CAPTION); w32(0); w16(1);
    w16(0); w16(0); w16(100); w16(50);
    w16(0); w16(0); wstr("P"); w16(8); wstr("DejaVu Sans");
    al4();
    w32(WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON); w32(0);
    w16(10); w16(10); w16(50); w16(14); w16(100);
    w16(0xFFFF); w16(0x0080); wstr("OK"); w16(0);

    HWND h = CreateDialogIndirectParamA(GetModuleHandleA(0), (LPCDLGTEMPLATE)tpl, NULL, Proc, 0);
    if (!h) { printf("no dialog\n"); return 1; }
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);          /* forces the paint/composite path */
    DestroyWindow(h);
    printf("done\n");
    return 0;
}
