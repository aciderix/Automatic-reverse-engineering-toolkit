/* _controlfp / _controlfp_s — the msvcrt floating-point control word (platform-
 * independent encoding). The process default is 0x0008001f (all exceptions masked,
 * 53-bit precision, round-to-nearest), measured from Wine. Stateful: a set updates
 * (cur & ~mask)|(new & mask) and returns the new word; a query (mask 0) returns the
 * current word. ARET matches Wine's observable values. Expected identical. */
#include <float.h>
#include <stdio.h>
int main(void) {
    unsigned q = _controlfp(0, 0);                 /* query default */
    unsigned s = _controlfp(_RC_UP, _MCW_RC);      /* set rounding up */
    unsigned q2 = _controlfp(0, 0);                /* confirm persisted */
    unsigned r = _controlfp(_CW_DEFAULT, 0xfffff); /* restore */
    unsigned p = _controlfp(_PC_24, _MCW_PC);      /* set precision -> new word */
    printf("query=%08x setRC=%08x q2=%08x restore=%08x setPC=%08x\n", q, s, q2, r, p);
    return 0;
}
