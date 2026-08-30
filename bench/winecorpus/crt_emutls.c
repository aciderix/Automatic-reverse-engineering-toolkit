/* libgcc emulated-TLS (__emutls_get_address): `__thread` on mingw i686 has no native
 * TLS, so with -shared-libgcc every access imports __emutls_get_address from
 * libgcc_s_dw2-1.dll. ARET routes it to its shim while the Wine oracle loads the real
 * libgcc_s (shim-vs-redist). Exercises initialised and zero-init __thread objects of
 * several sizes/alignments, plus persistence across accesses. Deterministic output. */
#include <stdio.h>
#include <string.h>
static __thread int t_int = 111;
static __thread long long t_ll = 0x1122334455667788LL;
static __thread char t_str[8] = "hi";
static __thread double t_dbl = 2.5;          /* 8-byte aligned */
static __thread int t_zero;                  /* no initialiser -> zero (templ==0) */
static __thread char t_big[64];              /* larger zero-init block */
int main(void){
    t_int += 5;
    t_ll  += 1;
    t_str[2] = '!'; t_str[3] = 0;
    t_dbl *= 2.0;
    t_zero += 7;
    memcpy(t_big, "abc", 4);
    printf("int=%d ll=%llx str=%s dbl=%.1f zero=%d big=%s\n",
           t_int, (unsigned long long)t_ll, t_str, t_dbl, t_zero, t_big);
    /* persistence: a second access must see the same block, not a re-init */
    t_int += 100;
    printf("persist int=%d ll=%llx\n", t_int, (unsigned long long)t_ll);
    printf("done\n");
    return 0;
}
