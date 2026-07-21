/* comctl32 socle batch 4: window navigation + parent/child reparenting. Exercises the
 * subset of the batch-4 shims that Wine gives a deterministic headless oracle for —
 * EnumChildWindows (child count), SetParent (returns old parent, GetParent reflects the
 * move), ChildWindowFromPoint (client-point hit-test). The resource/no-op shims in the
 * same batch (LoadStringW, GetNearestColor, ShowScrollBar, MapVirtualKeyW, ...) have no
 * cross-engine oracle here and are covered by the walls measurement, not this fixture.
 * Bit-identical to Wine. */
#include <windows.h>
#include <stdio.h>
static int g_cnt=0;
static LRESULT CALLBACK WP(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProc(h,m,w,l);}
static BOOL CALLBACK Enum(HWND h,LPARAM l){ (void)h;(void)l; g_cnt++; return TRUE; }
int main(void){
 WNDCLASSA wc;memset(&wc,0,sizeof wc);wc.lpfnWndProc=WP;wc.hInstance=GetModuleHandleA(0);wc.lpszClassName="NW";RegisterClassA(&wc);
 HWND p1=CreateWindowExA(0,"NW","p1",WS_OVERLAPPEDWINDOW,0,0,200,150,NULL,NULL,GetModuleHandleA(0),NULL);
 HWND p2=CreateWindowExA(0,"NW","p2",WS_OVERLAPPEDWINDOW,0,0,200,150,NULL,NULL,GetModuleHandleA(0),NULL);
 HWND c=CreateWindowExA(0,"BUTTON","b",WS_CHILD|WS_VISIBLE,10,20,50,20,p1,0,GetModuleHandleA(0),NULL);
 HWND c2=CreateWindowExA(0,"BUTTON","b2",WS_CHILD|WS_VISIBLE,70,20,50,20,p1,0,GetModuleHandleA(0),NULL);
 (void)c2;
 g_cnt=0; EnumChildWindows(p1,Enum,0); printf("enum=%d\n",g_cnt);
 HWND old=SetParent(c,p2); printf("setparent old_is_p1=%d newparent_is_p2=%d\n",old==p1,GetParent(c)==p2);
 POINT pt={80,25}; HWND hit=ChildWindowFromPoint(p1,pt);
 printf("childfrompt_is_c2=%d\n",hit==c2);
 DestroyWindow(p1);DestroyWindow(p2);
 printf("done\n");return 0;
}
