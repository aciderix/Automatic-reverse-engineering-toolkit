/* comctl32 socle batch 7: StretchBlt + SetDIBits + GdiAlphaBlend (+ pens). All reuse the
 * proven 32bpp DIB model; semantics measured bit-exact vs Wine:
 *   - StretchBlt: nearest-neighbour (src = s0 + i*sw/dw) — a 2x2 -> 4x4 upscale;
 *   - SetDIBits: bottom-up DIB rows map to image row y = H-1-scan;
 *   - GdiAlphaBlend: out = (src*ca + dst*(255-ca))/255 (constant SourceConstantAlpha).
 * Verified by reading back GetPixel on both engines. Windowed (GetDC(NULL)/DIBSections
 * need the Xvfb display). Bit-identical to Wine. */
#include <windows.h>
#include <stdio.h>
static HDC mkdib(int w,int h,void**bits){
    HDC dc=CreateCompatibleDC(GetDC(NULL));
    BITMAPINFO bi; memset(&bi,0,sizeof bi);
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth=w; bi.bmiHeader.biHeight=-h; /* top-down */
    bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
    HBITMAP bm=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,bits,NULL,0); SelectObject(dc,bm);
    return dc;
}
int main(void){
    unsigned *sb; HDC sdc=mkdib(2,2,(void**)&sb);
    sb[0]=0x00FF0000; sb[1]=0x0000FF00; sb[2]=0x000000FF; sb[3]=0x00FFFFFF;
    unsigned *db; HDC ddc=mkdib(4,4,(void**)&db);
    StretchBlt(ddc,0,0,4,4,sdc,0,0,2,2,SRCCOPY);
    printf("stretch (0,0)=%06lX (3,0)=%06lX (0,3)=%06lX (3,3)=%06lX (1,1)=%06lX\n",
        (unsigned long)GetPixel(ddc,0,0),(unsigned long)GetPixel(ddc,3,0),
        (unsigned long)GetPixel(ddc,0,3),(unsigned long)GetPixel(ddc,3,3),(unsigned long)GetPixel(ddc,1,1));

    HDC bdc=CreateCompatibleDC(GetDC(NULL));
    HBITMAP ddb=CreateCompatibleBitmap(GetDC(NULL),2,2);
    BITMAPINFO bi; memset(&bi,0,sizeof bi);
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=2;bi.bmiHeader.biHeight=2; /* bottom-up */
    bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    unsigned src[4]={0x00AA0000,0x0000BB00,0x000000CC,0x00DDDDDD};
    int n=SetDIBits(bdc,ddb,0,2,src,&bi,DIB_RGB_COLORS);
    SelectObject(bdc,ddb);
    printf("setdib n=%d (0,0)=%06lX (1,0)=%06lX (0,1)=%06lX (1,1)=%06lX\n",n,
        (unsigned long)GetPixel(bdc,0,0),(unsigned long)GetPixel(bdc,1,0),
        (unsigned long)GetPixel(bdc,0,1),(unsigned long)GetPixel(bdc,1,1));

    unsigned *ab; HDC adc=mkdib(2,2,(void**)&ab); ab[0]=ab[1]=ab[2]=ab[3]=0x00FF0000;
    unsigned *bb; HDC abd=mkdib(2,2,(void**)&bb); bb[0]=bb[1]=bb[2]=bb[3]=0;
    BLENDFUNCTION bf={AC_SRC_OVER,0,128,0};
    GdiAlphaBlend(abd,0,0,2,2,adc,0,0,2,2,bf);
    printf("alpha const128 = %06lX\n",(unsigned long)GetPixel(abd,0,0));
    printf("done\n");return 0;
}
