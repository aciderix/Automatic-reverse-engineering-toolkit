/* LEVIER 1 sur une VRAIE DLL BINAIRE SHIPPÉE (doc 80 §1.2, doc 70 §5.0).
 * ARET LIFTE la zlib1.dll de Wine (96 Ko de vrai code DEFLATE/inflate/crc32 —
 * mesurée: 0 thunk, 0 forwarder, imports kernel32+msvcrt seulement, donc du VRAI
 * code, pas un relais-stub comme version/shlwapi). Ses exports (compress/uncompress/
 * crc32/adler32/...) dispatchent vers le CODE LIFTÉ via le loader multi-modules ;
 * sous Wine (l'oracle) la MÊME zlib1.dll est chargée. Un aller-retour de compression
 * + les checksums sont ENTIÈREMENT DÉTERMINISTES (même code des deux côtés) ⇒
 * bit-identique. C'est la 1re DLL binaire tierce à ALGORITHME RÉEL prouvée sous ARET
 * (au-delà des DLL-fixtures qu'on compile nous-mêmes et des builtins registre/comctl32).
 *
 * Note §0: zlib dispatche crc32 sur pclmulqdq/SSE4.2 selon CPUID ; ARET masque ces ISA
 * (doc §4.1) ⇒ chemin scalaire liftable, tandis que Wine prend le SIMD — mais zlib
 * garantit une SORTIE identique quel que soit le chemin, donc les checksums matchent. */
#include <stdio.h>
#include <string.h>
typedef unsigned long uLong; typedef unsigned char Bytef; typedef unsigned int uInt;
__declspec(dllimport) int   compress(Bytef*, uLong*, const Bytef*, uLong);
__declspec(dllimport) int   uncompress(Bytef*, uLong*, const Bytef*, uLong);
__declspec(dllimport) uLong crc32(uLong, const Bytef*, uInt);
__declspec(dllimport) uLong adler32(uLong, const Bytef*, uInt);
__declspec(dllimport) uLong compressBound(uLong);
__declspec(dllimport) const char* zlibVersion(void);

/* Round-trip one buffer through compress()/uncompress(); print the deterministic
 * facts: input checksums, compressed length + first bytes, and that it restores. */
static void roundtrip(const char* tag, const Bytef* in, uLong n) {
    uLong crc_in = crc32(0, in, (uInt)n), adl_in = adler32(1, in, (uInt)n);
    uLong bound  = compressBound(n);
    Bytef comp[4096]; uLong clen = sizeof comp;
    int rc = compress(comp, &clen, in, n);
    Bytef out[2048]; uLong olen = sizeof out;
    int rc2 = uncompress(out, &olen, comp, clen);
    int same = (olen == n) && (memcmp(in, out, n) == 0);
    printf("%s: crc=%08lx adler=%08lx bound=%lu clen=%lu rc=%d/%d olen=%lu rt=%d comp=",
           tag, crc_in, adl_in, bound, clen, rc, rc2, olen, same);
    for (uLong i = 0; i < 12 && i < clen; i++) printf("%02x", comp[i]);
    printf("\n");
}

int main(void) {
    printf("zlib version %s\n", zlibVersion());

    /* (a) pseudo-random-ish bytes: low redundancy -> mostly literals/dynamic Huffman */
    Bytef rnd[512];
    for (int i = 0; i < 512; i++) rnd[i] = (Bytef)('A' + (i * 7 + i / 13) % 26);
    roundtrip("rand", rnd, sizeof rnd);

    /* (b) highly compressible: a repeated phrase -> long matches, small output */
    Bytef rep[1024];
    const char* p = "ARET lifts the real zlib1.dll and it runs bit-identical to Wine. ";
    for (int i = 0; i < 1024; i++) rep[i] = (Bytef)p[i % 64];
    roundtrip("repeat", rep, sizeof rep);

    /* (c) all-zero: run-length friendly, exercises the stored/fixed path edge */
    Bytef zero[256]; memset(zero, 0, sizeof zero);
    roundtrip("zeros", zero, sizeof zero);

    return 0;
}
