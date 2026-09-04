/* gdk-pixbuf maturity test — decode a PNG via the BUILT-IN png loader (C path:
   gdk_pixbuf -> libpng16 -> zlib), in-memory (no filesystem, no loaders.cache).
   Answers "how far does ARET reach on a real large C library after the EH fix". */
#include <stdio.h>
typedef int gboolean; typedef unsigned char guchar;
typedef void GdkPixbufLoader; typedef void GdkPixbuf; typedef void GError;
extern GdkPixbufLoader* gdk_pixbuf_loader_new(void);
extern gboolean gdk_pixbuf_loader_write(GdkPixbufLoader*, const guchar*, unsigned long, GError**);
extern gboolean gdk_pixbuf_loader_close(GdkPixbufLoader*, GError**);
extern GdkPixbuf* gdk_pixbuf_loader_get_pixbuf(GdkPixbufLoader*);
extern guchar* gdk_pixbuf_get_pixels(GdkPixbuf*);
extern int gdk_pixbuf_get_width(GdkPixbuf*);
extern int gdk_pixbuf_get_height(GdkPixbuf*);
extern int gdk_pixbuf_get_rowstride(GdkPixbuf*);
extern int gdk_pixbuf_get_n_channels(GdkPixbuf*);

static const unsigned char PNG[104] = {
137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,4,0,0,0,3,8,2,0,0,0,59,150,57,145,0,0,0,47,73,68,65,84,120,218,99,96,96,96,208,96,21,12,224,82,170,224,55,102,96,180,17,212,116,84,10,116,51,174,244,118,97,96,170,80,210,170,53,14,106,114,169,106,15,5,0,114,189,8,5,200,7,208,227,0,0,0,0,73,69,78,68,174,66,96,130
};

int main(void){
    GdkPixbufLoader *ld = gdk_pixbuf_loader_new();
    if(!ld){ printf("loader_new failed\n"); return 1; }
    if(!gdk_pixbuf_loader_write(ld, PNG, sizeof PNG, 0)){ printf("write failed\n"); return 1; }
    if(!gdk_pixbuf_loader_close(ld, 0)){ printf("close failed\n"); return 1; }
    GdkPixbuf *pb = gdk_pixbuf_loader_get_pixbuf(ld);
    if(!pb){ printf("get_pixbuf null\n"); return 1; }
    int w=gdk_pixbuf_get_width(pb), h=gdk_pixbuf_get_height(pb);
    int rs=gdk_pixbuf_get_rowstride(pb), nc=gdk_pixbuf_get_n_channels(pb);
    guchar *px=gdk_pixbuf_get_pixels(pb);
    unsigned long hsum=1469598103u;
    for(int y=0;y<h;y++) for(int x=0;x<w*nc;x++){ hsum^=px[y*rs+x]; hsum*=16777619u; hsum&=0xffffffffu; }
    printf("w=%d h=%d nc=%d hash=%08lx\n", w, h, nc, hsum);
    return 0;
}
