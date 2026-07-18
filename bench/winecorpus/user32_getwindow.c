/* GetWindow / GetTopWindow — navigate the window hierarchy. Children and siblings
 * share a parent and are ordered by creation, which matches Wine's default child
 * Z-order (create c1,c2,c3 under a -> GW_CHILD=c1, GW_HWNDNEXT(c1)=c2, GW_HWNDLAST=c3,
 * GW_HWNDFIRST(c3)=c1, GetTopWindow(a)=c1). Display-free (needs Xvfb only for Wine's
 * GDI init). Expected identical Wine and ARET: gwchild=1 next=2 last=3 first=1 topwin=1. */
#include <windows.h>
#include <stdio.h>
static const char* CLS="AretGW2";
static LRESULT CALLBACK WP(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProcA(h,m,w,l);}
int main(void){
    WNDCLASSA wc; memset(&wc,0,sizeof wc); wc.lpfnWndProc=WP; wc.hInstance=GetModuleHandleA(0); wc.lpszClassName=CLS;
    RegisterClassA(&wc);
    HWND a=CreateWindowExA(0,CLS,"a",WS_OVERLAPPEDWINDOW,0,0,50,50,0,0,wc.hInstance,0);
    HWND c1=CreateWindowExA(0,CLS,"c1",WS_CHILD,0,0,10,10,a,(HMENU)1,wc.hInstance,0);
    HWND c2=CreateWindowExA(0,CLS,"c2",WS_CHILD,0,0,10,10,a,(HMENU)2,wc.hInstance,0);
    HWND c3=CreateWindowExA(0,CLS,"c3",WS_CHILD,0,0,10,10,a,(HMENU)3,wc.hInstance,0);
    /* which child is GW_CHILD / top? and sibling order via GW_HWNDNEXT from there */
    HWND top=GetWindow(a,GW_CHILD);
    int which_top = top==c1?1: top==c2?2: top==c3?3: 0;
    HWND nxt = top?GetWindow(top,GW_HWNDNEXT):0;
    int which_nxt = nxt==c1?1: nxt==c2?2: nxt==c3?3: 0;
    HWND last=GetWindow(top,GW_HWNDLAST);
    int which_last = last==c1?1: last==c2?2: last==c3?3: 0;
    HWND first=GetWindow(c3,GW_HWNDFIRST);
    int which_first = first==c1?1: first==c2?2: first==c3?3: 0;
    printf("gwchild=%d next=%d last=%d first=%d topwin=%d\n", which_top, which_nxt, which_last, which_first,
        GetTopWindow(a)==top);
    return 0;
}
