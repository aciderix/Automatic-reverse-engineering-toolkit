/* TranslateCharsetInfo — three cells where Wine may not be the right answer.
 *
 * The gdi32 charset table was measured exhaustively against Wine and shipped
 * (winecorpus/gdi_charsetinfo.c). Three things in that measurement look like Wine
 * rather than like Win32, and Wine cannot settle them because Wine is the suspect:
 *
 *   1. `fsUsb[4]` comes back **all zero** from Wine. Windows documents fsUsb as the
 *      Unicode subset bitfield, so a real system plausibly fills it. If it does,
 *      every program that reads fsUsb to pick a font gets zeros from us.
 *   2. Wine accepts **charset 254 <-> code page 65001** (UTF-8). That entry is not
 *      a documented Windows charset; if Windows rejects it, our table has a row
 *      that answers where Windows fails.
 *   3. `TCI_SRCLOCALE` returns FALSE under Wine (it is a FIXME in its source). If
 *      Windows resolves it, our FALSE is a defined failure on a path that succeeds
 *      on a real system — the same shape as the CoCreateInstance trap.
 *
 * Everything is printed in full, and rejections are printed against a POISONED
 * buffer, because "returns FALSE" and "returns FALSE without writing" are two
 * different contracts. The last error is cleared before every call. */
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
    printf("%-22s ok=%d err=%lu cs=%u acp=%u usb=%08lx,%08lx,%08lx,%08lx csb=%08lx,%08lx\n",
           what, ok, (unsigned long)err, ci.ciCharset, ci.ciACP,
           (unsigned long)ci.fs.fsUsb[0], (unsigned long)ci.fs.fsUsb[1],
           (unsigned long)ci.fs.fsUsb[2], (unsigned long)ci.fs.fsUsb[3],
           (unsigned long)ci.fs.fsCsb[0], (unsigned long)ci.fs.fsCsb[1]);
}

int main(void)
{
    /* 1. Does fsUsb carry anything? Latin-1 and CJK would have very different
     *    subset bits if it does, so two rows discriminate. */
    one("charset ANSI(0)",     (DWORD *)(ULONG_PTR)0,   TCI_SRCCHARSET);
    one("charset SHIFTJIS",    (DWORD *)(ULONG_PTR)128, TCI_SRCCHARSET);
    one("cp 1252",             (DWORD *)(ULONG_PTR)1252, TCI_SRCCODEPAGE);

    /* 2. The UTF-8 row Wine has. */
    one("charset 254",         (DWORD *)(ULONG_PTR)254,   TCI_SRCCHARSET);
    one("cp 65001",            (DWORD *)(ULONG_PTR)65001, TCI_SRCCODEPAGE);

    /* 3. TCI_SRCLOCALE — Wine says FALSE. */
    one("locale 0x0409",       (DWORD *)(ULONG_PTR)0x0409, TCI_SRCLOCALE);
    one("locale 0x0411",       (DWORD *)(ULONG_PTR)0x0411, TCI_SRCLOCALE);

    /* Bonus rows: the two rejections whose "buffer untouched" property we rely on,
     * and the lowest-set-bit rule for TCI_SRCFONTSIG. */
    one("charset DEFAULT(1)",  (DWORD *)(ULONG_PTR)1, TCI_SRCCHARSET);
    { DWORD sig[2] = { 3u, 0 };          one("fontsig bits 0+1", sig, TCI_SRCFONTSIG); }
    { DWORD sig[2] = { 0, 0xffffffffu }; one("fontsig hi word only", sig, TCI_SRCFONTSIG); }
    return 0;
}
