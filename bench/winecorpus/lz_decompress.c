/* M7 G7 — LZ decompression (lzexpand.dll / SZDD). Write an embedded SZDD blob to
 * a file, then decompress it two ways (LZOpenFile+LZRead, and LZCopy to a file)
 * and check the bytes — a genuine deterministic LZSS decompressor, bit-identical
 * to Wine. Self-contained (the compressed blob is embedded; no external input). */
#include <windows.h>
#include <stdio.h>
static const unsigned char SZDD[] = { 83,90,68,68,136,240,39,51,65,0,53,0,0,0,255,72,101,108,108,111,32,65,82,255,69,84,32,45,45,32,76,90,255,32,83,90,68,68,32,100,101,255,99,111,109,112,114,101,115,115,255,105,111,110,32,119,111,114,107,255,115,32,101,110,100,32,116,111,31,32,101,110,100,33 };
int main(void) {
    HFILE h = _lcreat("t.sz_", 0);
    _lwrite(h, (const char *)SZDD, sizeof SZDD);
    _lclose(h);
    OFSTRUCT ofs;
    HFILE lz = LZOpenFileA("t.sz_", &ofs, OF_READ);
    printf("open=%d\n", lz != HFILE_ERROR);
    char buf[128]; memset(buf, 0, sizeof buf);
    LONG n = LZRead(lz, buf, sizeof buf - 1);
    printf("read=%ld <%s>\n", (long)n, buf);
    LZClose(lz);
    /* LZCopy to a file, then read it back */
    HFILE d = _lcreat("t.out", 0);
    HFILE s = LZOpenFileA("t.sz_", &ofs, OF_READ);
    LONG c = LZCopy(s, d);
    LZClose(s); _lclose(d);
    printf("copy=%ld\n", (long)c);
    HFILE r = _lopen("t.out", OF_READ);
    char buf2[128]; memset(buf2, 0, sizeof buf2);
    int rn = _lread(r, buf2, sizeof buf2 - 1);
    _lclose(r);
    printf("copied=%d <%s>\n", rn, buf2);
    printf("done\n");
    return 0;
}
