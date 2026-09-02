#include <stdio.h>
/* Native Windows implicit TLS, toolchain-independent. Data lives in the PE .tls
   image and is reached through the exact native sequence the loader must support:
     mov eax,_tls_index ; mov edx,fs:[0x2c] ; mov edx,[edx+eax*4] ; [edx + tv@SECREL32]
   mingw's tlssup.o supplies the scaffolding (__tls_index/__tls_start/__tls_used and
   the PE TLS directory) once referenced, so this exercises real static TLS even
   though gcc lowers __thread to emutls here. This is the class the cairo/pixman
   maturity test crashed on (fs:[0x2c]=0, _tls_index unset). Asm symbols carry the
   i686 mangling: C `tv` -> `_tv`, and tlssup exports `__tls_index`. */
__attribute__((section(".tls$BBB"), used)) int tv = 0x1234;

static int tls_read(void){
    int r;
    __asm__ volatile(
        "movl __tls_index, %%eax\n\t"
        "movl %%fs:0x2c, %%edx\n\t"
        "movl (%%edx,%%eax,4), %%edx\n\t"
        "movl _tv@SECREL32(%%edx), %0\n\t"
        : "=r"(r) : : "eax","edx");
    return r;
}
static void tls_write(int v){
    __asm__ volatile(
        "movl __tls_index, %%eax\n\t"
        "movl %%fs:0x2c, %%edx\n\t"
        "movl (%%edx,%%eax,4), %%edx\n\t"
        "movl %0, _tv@SECREL32(%%edx)\n\t"
        : : "r"(v) : "eax","edx");
}
int main(void){
    printf("a=%x\n", tls_read());
    tls_write(0xabcd);
    printf("b=%x\n", tls_read());
    return 0;
}
