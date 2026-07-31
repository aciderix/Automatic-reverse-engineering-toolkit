/* SPI_GETNONCLIENTMETRICS (SystemParametersInfo action 0x29) — the non-client metrics
 * and the five shell fonts a framework reads at start-up (the wall real MFC/WinMerge
 * hits during its GUI init). Both the A and the W structure are exercised: they are NOT
 * the same layout (LOGFONTA is 60 bytes, LOGFONTW 92), so a shim that forwards W to A
 * would silently write the wrong offsets.
 *
 * The whole structure is dumped as RAW BYTES, so every field — including the ones this
 * test does not name, and the padding — is compared against Wine, not just the handful
 * a typed printf would show. The buffer is pre-filled with a poison pattern so that
 * "left untouched" is observable too:
 *   - cbSize is the caller's field and selects the layout (uiParam is ignored);
 *   - the pre-Vista size (340/500) must leave iPaddedBorderWidth POISONED;
 *   - an unknown cbSize must return FALSE and write nothing at all.
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <stdio.h>

static void dump(const char *tag, int r, const unsigned char *p, unsigned n) {
    printf("%s r=%d bytes=%u\n", tag, r, n);
    for (unsigned i = 0; i < n; i++) {
        printf("%02x", p[i]);
        if ((i % 32) == 31 || i + 1 == n) printf("\n");
    }
}

int main(void) {
    printf("sizeA=%u sizeW=%u lfA=%u lfW=%u\n", (unsigned)sizeof(NONCLIENTMETRICSA),
           (unsigned)sizeof(NONCLIENTMETRICSW), (unsigned)sizeof(LOGFONTA),
           (unsigned)sizeof(LOGFONTW));

    /* A, modern size: every byte compared. */
    NONCLIENTMETRICSA a;
    memset(&a, 0xAB, sizeof a);
    a.cbSize = sizeof a;
    int ra = SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof a, &a, 0);
    dump("A-full", ra, (const unsigned char *)&a, (unsigned)sizeof a);

    /* W, modern size. */
    NONCLIENTMETRICSW w;
    memset(&w, 0xAB, sizeof w);
    w.cbSize = sizeof w;
    int rw = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof w, &w, 0);
    dump("W-full", rw, (const unsigned char *)&w, (unsigned)sizeof w);

    /* A, pre-Vista size: iPaddedBorderWidth must stay poisoned (0xABABABAB). */
    NONCLIENTMETRICSA p;
    memset(&p, 0xAB, sizeof p);
    p.cbSize = (UINT)(sizeof p - sizeof(int));
    int rp = SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, p.cbSize, &p, 0);
    dump("A-preVista", rp, (const unsigned char *)&p, (unsigned)sizeof p);

    /* Unknown cbSize: FALSE, and nothing written (all bytes stay poisoned). */
    NONCLIENTMETRICSA b;
    memset(&b, 0xAB, sizeof b);
    b.cbSize = 123;
    int rb = SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, 123, &b, 0);
    unsigned poisoned = 0;
    for (unsigned i = 4; i < sizeof b; i++)
        if (((const unsigned char *)&b)[i] == 0xAB) poisoned++;
    printf("A-bogus r=%d untouched=%u/%u\n", rb, poisoned, (unsigned)sizeof b - 4);

    /* uiParam is ignored — cbSize alone selects the layout. */
    NONCLIENTMETRICSA u;
    memset(&u, 0xAB, sizeof u);
    u.cbSize = sizeof u;
    int ru = SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, 0, &u, 0);
    printf("A-uiParam0 r=%d border=%d capH=%d menuH=%d face=[%s]\n", ru, u.iBorderWidth,
           u.iCaptionHeight, u.iMenuHeight, u.lfCaptionFont.lfFaceName);
    return 0;
}
