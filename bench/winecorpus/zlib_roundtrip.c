/* zlib round-trip: deterministic compressed bytes (fixed input + level) so ARET
 * (lifting zlib1.dll) and Wine (the real DLL) match to the byte. Prints the
 * compressed length, a CRC32/adler32 over the compressed output, and verifies
 * uncompress restores the original. */
#include <zlib.h>
#include <stdio.h>
#include <string.h>
int main(void){
    unsigned char src[512];
    for (int i=0;i<512;i++) src[i]=(unsigned char)((i*37+11)^(i>>3));  /* fixed pattern */
    unsigned char comp[1024]; uLongf clen=sizeof comp;
    int rc=compress2(comp,&clen,src,sizeof src,9);
    printf("compress rc=%d clen=%lu\n",rc,(unsigned long)clen);
    unsigned long ccrc=crc32(0L,comp,(uInt)clen);
    unsigned long cadl=adler32(1L,comp,(uInt)clen);
    printf("comp_crc32=%08lx comp_adler=%08lx\n",ccrc,cadl);
    /* first 16 compressed bytes */
    printf("comp16=");for(int i=0;i<16 && i<(int)clen;i++)printf("%02x",comp[i]);printf("\n");
    unsigned char back[512]; uLongf blen=sizeof back;
    int ru=uncompress(back,&blen,comp,clen);
    printf("uncompress rc=%d blen=%lu match=%d\n",ru,(unsigned long)blen,
           blen==sizeof src && memcmp(back,src,sizeof src)==0);
    printf("zlib=%s\n",zlibVersion());
    printf("done\n");return 0;
}
