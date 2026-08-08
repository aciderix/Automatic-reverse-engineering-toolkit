/* LEVIER 1 sur le RUNTIME C++ GNU — la lacune n°1 mesurée (doc 90 : le corpus de
 * 1240 vrais PE32 FOSS pointe libgcc_s + libstdc++ comme le 1er mur, 37-47 % des
 * binaires). ARET LIFTE la VRAIE libgcc_s_dw2-1.dll de mingw (mesurée §0 : 0 thunk,
 * 0 forwarder, .text ~130 Ko de vrai code, imports KERNEL32+msvcrt seulement — du
 * VRAI code, pas un relais-stub). Ce fixture exerce ses HELPERS ARITHMÉTIQUES 64 bits
 * (__divdi3/__moddi3/__udivdi3/__umoddi3/__muldi3/__ashldi3/__lshrdi3/__ashrdi3 —
 * mesurés bloquants sur ~101 binaires du corpus). Les opérations int64 en C émettent
 * `call ___divdi3` etc., que lift_libgcc.def route en IMPORTS depuis la DLL ⇒ ARET les
 * dispatche vers le CODE LIFTÉ via le loader multi-modules ; sous Wine (l'oracle) la
 * MÊME libgcc est chargée à côté de l'exe. Tout est PUR et DÉTERMINISTE ⇒ bit-identique.
 *
 * C'est la 2e DLL binaire tierce à ALGORITHME RÉEL prouvée liftée (après zlib1.dll), et
 * la 1re marche du plan mesuré : libgcc est autonome (imports couverts) ; libstdc++-6.dll
 * (qui n'importe QUE libgcc+kernel32+msvcrt) se liftera ensuite PAR-DESSUS (multi-module). */
#include <stdio.h>

/* Discriminant grid: 0, ±1, big +/-, and the two int64 extremes (INT64_MIN/MAX) —
 * covers sign handling, the INT64_MIN/-1 overflow-defining case, and unsigned wrap. */
static long long S[] = { 0, 1, -1, 1234567890123LL, -9876543210987LL,
                         0x7fffffffffffffffLL, (long long)0x8000000000000000ULL };

int main(void) {
    int n = (int)(sizeof(S) / sizeof(S[0]));
    unsigned long long acc = 0xabcdef0123456789ULL;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            long long a = S[i], b = S[j];
            if (b) {                              /* __divdi3/__moddi3/__udivdi3/__umoddi3 */
                long long q = a / b, r = a % b;
                unsigned long long ua = (unsigned long long)a, ub = (unsigned long long)b;
                unsigned long long uq = ua / ub, ur = ua % ub;
                printf("a=%lld b=%lld q=%lld r=%lld uq=%llu ur=%llu\n", a, b, q, r, uq, ur);
                acc ^= (unsigned long long)q * 3 + r + uq - ur;   /* __muldi3 */
            }
            long long m = a * b;                  /* __muldi3 */
            acc += (unsigned long long)m;
            acc ^= (unsigned long long)a << (j & 63);   /* __ashldi3 */
            acc += (unsigned long long)a >> (i & 63);   /* __lshrdi3 / __ashrdi3 */
        }
    printf("acc=%016llx\n", acc);
    return 0;
}
