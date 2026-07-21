/* comctl32 socle batch 8: icon / cursor handle management. CreateIconIndirect keeps the
 * caller's colour/mask bitmaps so GetIconInfo round-trips — and reproduces Wine's rule
 * that GetIconInfo on an ICON reports the hotspot as the bitmap CENTRE (cx/2,cy/2),
 * ignoring the stored hotspot, and returns fresh bitmap copies with the right dims.
 * CopyImage deep-copies a bitmap (distinct handle, dims/bpp preserved, resized when a
 * size is given); CopyIcon returns a distinct icon; DestroyIcon frees it. The DRAWING
 * side (DrawIcon/DrawIconEx/DrawStateW) has no headless oracle and is verified
 * qualitatively (like the other on-screen paint). Windowed (GetDC(NULL) needs the Xvfb
 * display). Bit-identical to Wine. */
#include <windows.h>
#include <stdio.h>
int main(void){
    HDC sdc=GetDC(NULL);
    HBITMAP color=CreateCompatibleBitmap(sdc,16,16);
    HBITMAP mask=CreateBitmap(16,16,1,1,NULL);
    ICONINFO ii; ii.fIcon=TRUE; ii.xHotspot=5; ii.yHotspot=7; ii.hbmColor=color; ii.hbmMask=mask;
    HICON ic=CreateIconIndirect(&ii);
    printf("created=%d\n", ic!=NULL);
    ICONINFO o; memset(&o,0,sizeof o);
    int g=GetIconInfo(ic,&o);
    printf("getinfo=%d fIcon=%d hotx=%u hoty=%u color_nn=%d mask_nn=%d\n",
        g,o.fIcon,o.xHotspot,o.yHotspot,o.hbmColor!=NULL,o.hbmMask!=NULL);
    BITMAP bm; GetObject(o.hbmColor,sizeof bm,&bm);
    printf("color dims=%dx%d\n",bm.bmWidth,bm.bmHeight);
    HBITMAP orig=CreateBitmap(4,4,1,32,NULL);
    HBITMAP cp=(HBITMAP)CopyImage(orig,IMAGE_BITMAP,0,0,0);
    printf("copyimage_distinct=%d\n", cp!=orig && cp!=NULL);
    BITMAP cb; GetObject(cp,sizeof cb,&cb);
    printf("copy dims=%dx%d bpp=%d\n",cb.bmWidth,cb.bmHeight,cb.bmBitsPixel);
    HBITMAP cp2=(HBITMAP)CopyImage(orig,IMAGE_BITMAP,8,8,0);
    GetObject(cp2,sizeof cb,&cb);
    printf("copy8 dims=%dx%d\n",cb.bmWidth,cb.bmHeight);
    HICON ic2=CopyIcon(ic);
    printf("copyicon=%d distinct=%d\n", ic2!=NULL, ic2!=ic);
    printf("destroy=%d\n", DestroyIcon(ic2));
    printf("done\n");return 0;
}
