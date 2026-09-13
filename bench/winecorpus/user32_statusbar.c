/* comctl32 status bar (msctls_statusbar32), lifted from Wine. The part layout and text
   state are pure comctl32 state that round-trips bit-identically to Wine; the control's
   auto-computed HEIGHT is font-metric dependent (measured Wine 26 vs ARET 20) so it is
   deliberately NOT asserted here — a separate GDI font-metrics concern (KN). Companion
   .withdll lifts comctl32; Wine (oracle) loads its own. */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
static LRESULT CALLBACK wp(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProcW(h,m,w,l);}
int main(void){
    HINSTANCE hi=GetModuleHandleW(NULL);
    INITCOMMONCONTROLSEX ic={sizeof ic, ICC_BAR_CLASSES}; InitCommonControlsEx(&ic);
    WNDCLASSW wc={0}; wc.lpfnWndProc=wp; wc.hInstance=hi; wc.lpszClassName=L"SbarP";
    RegisterClassW(&wc);
    HWND h=CreateWindowExW(0,L"SbarP",L"s",WS_OVERLAPPEDWINDOW,0,0,400,300,NULL,NULL,hi,NULL);
    HWND sb=CreateWindowExW(0,STATUSCLASSNAMEW,NULL,WS_CHILD|WS_VISIBLE,0,0,0,0,h,(HMENU)1,hi,NULL);
    printf("sb_created=%d\n", sb!=NULL);
    int parts[3]={100,250,-1};
    SendMessageW(sb,SB_SETPARTS,3,(LPARAM)parts);
    int got[4]={0,0,0,0};
    int np=(int)SendMessageW(sb,SB_GETPARTS,3,(LPARAM)got);
    printf("nparts=%d edge0=%d edge1=%d\n", np, got[0], got[1]);
    SendMessageW(sb,SB_SETTEXTW,0,(LPARAM)L"Ready");
    SendMessageW(sb,SB_SETTEXTW,1,(LPARAM)L"Ln 1, Col 1");
    WCHAR b0[64]={0},b1[64]={0};
    int l0=(int)SendMessageW(sb,SB_GETTEXTW,0,(LPARAM)b0);
    int l1=(int)SendMessageW(sb,SB_GETTEXTW,1,(LPARAM)b1);
    int tl0=(int)SendMessageW(sb,SB_GETTEXTLENGTHW,0,0);
    printf("t0 len=%d tl=%d s=%ls\n", l0, tl0, b0);
    printf("t1 len=%d s=%ls\n", l1, b1);
    /* SB_GETRECT x-extents for part 0 (font-independent: from SB_SETPARTS boundary) */
    RECT r0={0,0,0,0}; SendMessageW(sb,SB_GETRECT,0,(LPARAM)&r0);
    printf("rect0 l=%ld r=%ld\n",(long)r0.left,(long)r0.right);
    printf("done\n"); return 0;
}
