/* comctl32 socle batch 6a: per-character metrics (FreeType). These share the exact DC
 * font path as GetTextExtentPoint32 (already bit-identical to Wine), so the advances
 * agree by construction: GetCharWidthW (per-char advance), GetCharABCWidthsW (A = left
 * bearing, B = black-box width, C = advance-A-B), GetTextExtentExPointW (full-string
 * extent + fit count + cumulative dx array). Uses "DejaVu Sans" so the font resolves
 * identically on both engines (same font-resolution caveat as the other text fixtures /
 * gdi_uifont). Windowed (GetDC(NULL) needs the Xvfb display). Bit-identical to Wine. */
#include <windows.h>
#include <stdio.h>
int main(void){
    HDC dc=CreateCompatibleDC(GetDC(NULL));
    LOGFONTA lf; memset(&lf,0,sizeof lf); lf.lfHeight=-16; strcpy(lf.lfFaceName,"DejaVu Sans");
    HFONT f=CreateFontIndirectA(&lf); SelectObject(dc,f);
    int cw[4]; GetCharWidthW(dc,'A','D',cw);
    printf("cw A-D: %d %d %d %d\n",cw[0],cw[1],cw[2],cw[3]);
    ABC abc[4]; GetCharABCWidthsW(dc,'A','D',abc);
    for(int i=0;i<4;i++) printf("abc[%c]=%d,%u,%d\n",'A'+i,abc[i].abcA,abc[i].abcB,abc[i].abcC);
    SIZE sz; int fit=0; int dx[8];
    WCHAR s[]={'A','B','C','D',0};
    GetTextExtentExPointW(dc,s,4,1000,&fit,dx,&sz);
    printf("extex all: fit=%d cx=%ld dx=%d,%d,%d,%d\n",fit,sz.cx,dx[0],dx[1],dx[2],dx[3]);
    GetTextExtentExPointW(dc,s,4,20,&fit,dx,&sz);
    printf("extex fit20: fit=%d cx=%ld\n",fit,sz.cx);
    printf("done\n");return 0;
}
