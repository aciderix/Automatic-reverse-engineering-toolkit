/* msvcrt character classification over the WHOLE 0..255 range.
 *
 * This exists to gate the ctype TABLE that _isctype answers from. _isctype itself
 * cannot be linked into a fixture — mingw's import library does not expose it, and
 * routing through GetProcAddress does not work either because ARET's GetProcAddress
 * always answers NULL (70 §P1quater). The standard isXXX entry points are importable
 * and classify from the same data, so exercising them pins down the part that can
 * actually be wrong: the table content.
 *
 * Every code from 0 to 255 is printed as one hex digit set, so a single wrong cell
 * shows up rather than hiding behind a spot check. The two cells most likely to be got
 * wrong by reasoning instead of measurement are covered by construction: the tab is
 * _SPACE but NOT _BLANK (unlike C's isblank), and codes 128..255 belong to no class at
 * all in the C locale.
 *
 * Expected identical under Wine and ARET. */
#include <stdio.h>
#include <ctype.h>

int main(void)
{
    for (int row = 0; row < 16; row++) {
        printf("%02x:", row * 16);
        for (int col = 0; col < 16; col++) {
            int c = row * 16 + col;
            unsigned v = 0;
            if (isupper(c))  v |= 0x01;
            if (islower(c))  v |= 0x02;
            if (isdigit(c))  v |= 0x04;
            if (isspace(c))  v |= 0x08;
            if (ispunct(c))  v |= 0x10;
            if (iscntrl(c))  v |= 0x20;
            if (isxdigit(c)) v |= 0x80;
            printf(" %03x", v);
        }
        puts("");
    }
    /* Aggregate counts: a table shifted by one would keep the shape above but change
     * these, so they are a cheap independent check on the same data. */
    int nu = 0, nl = 0, nd = 0, ns = 0, np = 0, nc = 0, nx = 0, na = 0;
    for (int c = 0; c < 256; c++) {
        nu += !!isupper(c);  nl += !!islower(c);  nd += !!isdigit(c);
        ns += !!isspace(c);  np += !!ispunct(c);  nc += !!iscntrl(c);
        nx += !!isxdigit(c); na += !!isalnum(c);
    }
    printf("upper=%d lower=%d digit=%d space=%d punct=%d cntrl=%d xdigit=%d alnum=%d\n",
           nu, nl, nd, ns, np, nc, nx, na);
    /* The tab: _SPACE yes, and C's isblank would also say yes — msvcrt's _BLANK bit
     * does not. Printed explicitly so the distinction is recorded by the gate. */
    printf("tab: space=%d cntrl=%d | space char: space=%d punct=%d\n",
           !!isspace('\t'), !!iscntrl('\t'), !!isspace(' '), !!ispunct(' '));
    return 0;
}
