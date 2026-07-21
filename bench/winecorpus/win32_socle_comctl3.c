/* comctl32 socle batch 3: GDI regions (rect-bounding model — CreateRectRgn/Indirect,
 * SetRectRgn, GetRgnBox, CombineRgn AND/OR/COPY with the correct SIMPLE/COMPLEX/NULL
 * type) + SubtractRect. Bit-identical to Wine on the rectangular cases (a non-rect OR
 * is COMPLEXREGION with the bounding box, as Wine reports). */
#include <windows.h>
#include <stdio.h>
static void pr(const char*n,HRGN r){ RECT b; int t=GetRgnBox(r,&b); printf("%s type=%d %ld %ld %ld %ld\n",n,t,(long)b.left,(long)b.top,(long)b.right,(long)b.bottom); }
int main(void){
 HRGN a=CreateRectRgn(10,10,50,40);
 HRGN b=CreateRectRgn(30,20,70,60);
 pr("a",a); pr("b",b);
 HRGN d=CreateRectRgn(0,0,0,0);
 CombineRgn(d,a,b,RGN_AND); pr("and",d);
 CombineRgn(d,a,b,RGN_OR);  pr("or",d);
 CombineRgn(d,a,b,RGN_COPY);pr("copy",d);
 SetRectRgn(a,1,2,3,4); pr("set",a);
 HRGN e=CreateRectRgn(0,0,0,0); HRGN f=CreateRectRgn(100,100,110,110);
 CombineRgn(e,f,b,RGN_AND); printf("disjoint_and=%d\n",GetRgnBox(e,&(RECT){0}));
 // SubtractRect: full-width top band removed
 RECT src={0,0,100,50}, sub={0,0,100,20}, out;
 int ok=SubtractRect(&out,&src,&sub);
 printf("sub ok=%d %ld %ld %ld %ld\n",ok,(long)out.left,(long)out.top,(long)out.right,(long)out.bottom);
 printf("done\n");return 0;
}
