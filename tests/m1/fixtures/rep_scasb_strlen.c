/* Forces the MSVC inline-strlen idiom `xor eax,eax; or ecx,-1; repne scasb;
   not ecx; dec ecx` that ARET must model (repne scasb). Prints the length so the
   transpiled run can be checked against the native run. */
#include <stdio.h>
static int asm_strlen(const char* s){
    int len;
    __asm__ volatile(
        "xorl %%eax, %%eax\n\t"
        "orl $-1, %%ecx\n\t"
        "repne scasb\n\t"
        "notl %%ecx\n\t"
        "decl %%ecx\n\t"
        "movl %%ecx, %0\n\t"
        : "=r"(len) : "D"(s) : "eax","ecx","cc");
    return len;
}
int main(void){
    const char* a = "SELECT 6+6;";
    const char* b = "hello";
    printf("len(a)=%d len(b)=%d len(empty)=%d\n", asm_strlen(a), asm_strlen(b), asm_strlen(""));
    return 0;
}
