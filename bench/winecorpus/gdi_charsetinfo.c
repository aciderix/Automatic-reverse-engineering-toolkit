/* TranslateCharsetInfo — the charset <-> code page <-> font-signature table.
 *
 * This fixture is deliberately EXHAUSTIVE, not a sample: all 256 charset values,
 * all 32 `fsCsb[0]` bits, and a code-page list covering every OEM/ANSI page a
 * Win32 program is likely to hand over. That matters for more than coverage — it
 * is what makes embedding this table legitimate at all (doc 70 §7): the data is
 * version-dependent but deterministic, so sweeping every cell means a change on
 * either side turns this red instead of rotting in silence.
 *
 * Every rejection is printed too, and printed with the raw bytes of a POISONED
 * CHARSETINFO, because "returns FALSE" and "returns FALSE without touching the
 * caller's structure" are different contracts and only the second is safe to rely
 * on. Same reason the last error is reset before each call: a probe that reads
 * global state must clear it first or it measures the previous call's residue.
 *
 * Values are read AFTER the call in their own statement — printf argument
 * evaluation order is unspecified, and reading the struct inside the same printf
 * that makes the call reports the PRE-call bytes (this bit me while measuring).
 *
 * NOT probed: a NULL destination. Wine faults on it (measured), so it is a caller
 * bug rather than a contract, and a fixture that crashes proves nothing.
 *
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <wingdi.h>
#include <stdio.h>
#include <string.h>

static void one(const char *what, DWORD *src, DWORD flags)
{
    CHARSETINFO ci;
    memset(&ci, 0xAA, sizeof ci);
    SetLastError(0);
    BOOL ok = TranslateCharsetInfo(src, &ci, flags);
    DWORD err = GetLastError();
    const unsigned char *raw = (const unsigned char *)&ci;
    printf("%-20s ok=%d err=%lu cs=%u acp=%u usb=%08lx,%08lx,%08lx,%08lx csb=%08lx,%08lx\n",
           what, ok, (unsigned long)err, ci.ciCharset, ci.ciACP,
           (unsigned long)ci.fs.fsUsb[0], (unsigned long)ci.fs.fsUsb[1],
           (unsigned long)ci.fs.fsUsb[2], (unsigned long)ci.fs.fsUsb[3],
           (unsigned long)ci.fs.fsCsb[0], (unsigned long)ci.fs.fsCsb[1]);
    if (!ok) {                       /* prove nothing at all was written */
        int intact = 1;
        for (unsigned i = 0; i < sizeof ci; i++) if (raw[i] != 0xAA) intact = 0;
        printf("%-20s   buffer-intact=%d\n", what, intact);
    }
}

int main(void)
{
    char nm[64];
    printf("sizeof(CHARSETINFO)=%u\n", (unsigned)sizeof(CHARSETINFO));

    for (unsigned cs = 0; cs < 256; cs++) {
        sprintf(nm, "charset %u", cs);
        one(nm, (DWORD *)(ULONG_PTR)cs, TCI_SRCCHARSET);
    }

    static const unsigned cps[] = { 0, 1, 42, 437, 708, 709, 710, 720, 737, 775, 850,
        852, 855, 857, 858, 860, 861, 862, 863, 864, 865, 866, 869, 874, 875, 932, 936,
        949, 950, 1200, 1250, 1251, 1252, 1253, 1254, 1255, 1256, 1257, 1258, 1361,
        10000, 20127, 28591, 65000, 65001, 99999 };
    for (unsigned i = 0; i < sizeof cps / sizeof cps[0]; i++) {
        sprintf(nm, "cp %u", cps[i]);
        one(nm, (DWORD *)(ULONG_PTR)cps[i], TCI_SRCCODEPAGE);
    }

    /* TCI_SRCFONTSIG: the source is a POINTER to fsCsb, not a value. */
    for (int bit = 0; bit < 32; bit++) {
        DWORD sig[2] = { 1u << bit, 0 };
        sprintf(nm, "fontsig bit %d", bit);
        one(nm, sig, TCI_SRCFONTSIG);
    }
    { DWORD sig[2] = { 3u, 0 };            one("fontsig bits 0+1", sig, TCI_SRCFONTSIG); }
    { DWORD sig[2] = { 0x80000001u, 0 };   one("fontsig bits 0+31", sig, TCI_SRCFONTSIG); }
    { DWORD sig[2] = { 0, 0 };             one("fontsig zero", sig, TCI_SRCFONTSIG); }
    { DWORD sig[2] = { 0, 0xffffffffu };   one("fontsig hi only", sig, TCI_SRCFONTSIG); }

    /* Flags outside the modelled set, including TCI_SRCLOCALE. */
    one("flag 0", (DWORD *)(ULONG_PTR)0, 0);
    one("flag 7", (DWORD *)(ULONG_PTR)0, 7);
    one("TCI_SRCLOCALE", (DWORD *)(ULONG_PTR)0x409, TCI_SRCLOCALE);
    return 0;
}
