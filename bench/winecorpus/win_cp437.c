/* OEM CP437 forward+reverse modelled (doc 82). kernel32 MultiByteToWideChar/WideCharToMultiByte
 * (CP_OEMCP) and ntdll RtlOemToUnicodeN/RtlUnicodeToOemN share the measured CP437 tables ->
 * bit-identical Wine incl. best-fit and default char. */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
NTSTATUS WINAPI RtlOemToUnicodeN(PWCH,ULONG,PULONG,PCCH,ULONG);
NTSTATUS WINAPI RtlUnicodeToOemN(PCHAR,ULONG,PULONG,PCWCH,ULONG);
int main(void){
    /* forward: every OEM byte 0x80..0xFF via kernel32 CP_OEMCP */
    char mb[0x81]; for(int i=0;i<0x80;i++) mb[i]=(char)(0x80+i); mb[0x80]=0;
    WCHAR wc[0x81]; int n=MultiByteToWideChar(437,0,mb,0x80,wc,0x80);
    printf("k32fwd n=%d:",n); for(int i=0;i<n;i++) printf(" %04X",wc[i]); printf("\n");
    /* forward via ntdll for the same bytes */
    WCHAR nw[0x81]; ULONG nn=0; RtlOemToUnicodeN(nw,sizeof nw,&nn,(PCCH)mb,0x80);
    printf("ntfwd n=%lu:",(unsigned long)nn/2); for(unsigned i=0;i<nn/2;i++) printf(" %04X",nw[i]); printf("\n");
    /* reverse: a spread of code points via kernel32 CP_OEMCP + ntdll */
    unsigned short cps[]={0x00C7,0x00FC,0x2593,0x03B1,0x0041,0x00E9,0x221E,0x2264,0x4E2D,0x00BD};
    int k=sizeof cps/sizeof cps[0];
    char kb[32]; BOOL used=0; int kn=WideCharToMultiByte(437,0,(WCHAR*)cps,k,kb,sizeof kb,NULL,&used);
    printf("k32rev n=%d used=%d:",kn,used); for(int i=0;i<kn;i++) printf(" %02X",(unsigned char)kb[i]); printf("\n");
    char ob[32]; ULONG on=0; RtlUnicodeToOemN(ob,sizeof ob,&on,(PCWCH)cps,(ULONG)(k*2));
    printf("ntrev n=%lu:",(unsigned long)on); for(unsigned i=0;i<on;i++) printf(" %02X",(unsigned char)ob[i]); printf("\n");
    return 0;
}
