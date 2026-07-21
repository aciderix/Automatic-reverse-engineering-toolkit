/* Hardening of five socle shims that used to be lazy no-op/constants, each now MEASURED
 * bit-identical to Wine (self-audit follow-up — replacing "plausible defaults" with real,
 * verified behaviour):
 *   - IsValidLocale: valid iff the primary language id is in the assigned MS-LCID range
 *     (no longer "always TRUE"); 0/0xFFFF/USER_DEFAULT/SYSTEM_DEFAULT rejected as Wine does;
 *   - MapVirtualKey: the US keyboard scan-code table (VK<->VSC, VK->CHAR);
 *   - ShowScrollBar: toggles the WS_HSCROLL/WS_VSCROLL style bit;
 *   - GetClassLong: the real registered class fields (window -> class registry);
 *   - ScrollWindowEx: the newly-exposed update strip (on a child, client == window).
 * Windowed (needs the Xvfb display). Bit-identical to Wine. */
#include <windows.h>
#include <stdio.h>
static LRESULT CALLBACK WP(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProc(h,m,w,l);}
int main(void){
    printf("ivl 0409=%d 040C=%d 007F=%d 0000=%d FFFF=%d 0400=%d 00A0=%d 0091=%d\n",
        IsValidLocale(0x0409,2),IsValidLocale(0x040C,2),IsValidLocale(0x007F,2),IsValidLocale(0,2),
        IsValidLocale(0xFFFF,2),IsValidLocale(0x0400,2),IsValidLocale(0x00A0,2),IsValidLocale(0x0091,2));
    printf("mvk A=%u RET=%u SHIFT=%u F1=%u | Achar=%u 1E->vk=%u\n",
        MapVirtualKeyA('A',0),MapVirtualKeyA(VK_RETURN,0),MapVirtualKeyA(VK_SHIFT,0),MapVirtualKeyA(VK_F1,0),
        MapVirtualKeyA('A',2),MapVirtualKeyA(0x1E,1));

    WNDCLASSA wc; memset(&wc,0,sizeof wc); wc.lpfnWndProc=WP; wc.hInstance=GetModuleHandleA(0);
    wc.lpszClassName="SH"; wc.style=0x0003; wc.cbClsExtra=8; wc.cbWndExtra=12;
    wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); RegisterClassA(&wc);
    HWND h=CreateWindowExA(0,"SH","t",WS_OVERLAPPEDWINDOW|WS_HSCROLL|WS_VSCROLL|WS_VISIBLE,0,0,300,200,NULL,NULL,GetModuleHandleA(0),NULL);
    printf("gcl style=%lu cbcls=%ld cbwnd=%ld hbr_nz=%d hmod_ok=%d\n",
        (unsigned long)GetClassLongA(h,GCL_STYLE),(long)GetClassLongA(h,GCL_CBCLSEXTRA),(long)GetClassLongA(h,GCL_CBWNDEXTRA),
        GetClassLongA(h,GCL_HBRBACKGROUND)!=0,(HMODULE)GetClassLongA(h,GCL_HMODULE)==GetModuleHandleA(0));
    ShowScrollBar(h,SB_HORZ,FALSE);
    printf("ssb hide_h: h=%d v=%d\n",!!(GetWindowLongA(h,GWL_STYLE)&WS_HSCROLL),!!(GetWindowLongA(h,GWL_STYLE)&WS_VSCROLL));
    ShowScrollBar(h,SB_VERT,FALSE); ShowScrollBar(h,SB_HORZ,TRUE);
    printf("ssb toggle: h=%d v=%d\n",!!(GetWindowLongA(h,GWL_STYLE)&WS_HSCROLL),!!(GetWindowLongA(h,GWL_STYLE)&WS_VSCROLL));

    HWND ch=CreateWindowExA(0,"SH","c",WS_CHILD|WS_VISIBLE,0,0,120,80,h,0,GetModuleHandleA(0),NULL);
    RECT u1={0},u2={0},u3={0};
    int r1=ScrollWindowEx(ch,10,0,NULL,NULL,NULL,&u1,0);
    int r2=ScrollWindowEx(ch,0,-15,NULL,NULL,NULL,&u2,0);
    int r3=ScrollWindowEx(ch,-8,12,NULL,NULL,NULL,&u3,0);
    printf("swex r=%d,%d,%d u1=%ld,%ld,%ld,%ld u2=%ld,%ld,%ld,%ld u3=%ld,%ld,%ld,%ld\n",r1,r2,r3,
        (long)u1.left,(long)u1.top,(long)u1.right,(long)u1.bottom,
        (long)u2.left,(long)u2.top,(long)u2.right,(long)u2.bottom,
        (long)u3.left,(long)u3.top,(long)u3.right,(long)u3.bottom);
    printf("done\n");return 0;
}
