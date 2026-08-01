/* ConvertDefaultLocale — the modelled domain, swept exhaustively.
 *
 * The five "default" pseudo-LCIDs resolve to the user locale; every LCID with a
 * non-neutral sublang passes through unchanged, with 33 documented exceptions that
 * ARET does not model (they are Wine's locale table, environmental data rather than a
 * rule, so they abort instead of shipping a guess) and are therefore excluded here.
 *
 * The sweep is the point. A spot check of a handful of LCIDs would keep passing if
 * Wine's table gained or lost an entry inside the range we claim to pass through, and
 * that is exactly the change that would make our model silently wrong. Walking all
 * 64416 of them turns that into a visible failure. The per-value results are folded
 * into a checksum rather than printed, so the fixture stays readable while still
 * depending on every one of them.
 *
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <stdio.h>

/* The LCIDs Wine remaps: outside the modelled domain, skipped by the sweep. */
static const unsigned remapped[] = {
    0x0460, 0x641a, 0x681a, 0x6c1a, 0x701a, 0x703b, 0x742c, 0x743b, 0x7804,
    0x7814, 0x781a, 0x782c, 0x783b, 0x7843, 0x7850, 0x785d, 0x7c04, 0x7c14,
    0x7c1a, 0x7c28, 0x7c2e, 0x7c3b, 0x7c43, 0x7c46, 0x7c50, 0x7c59, 0x7c5c,
    0x7c5d, 0x7c5f, 0x7c67, 0x7c68, 0x7c86, 0x7c92,
};
static int is_remapped(unsigned l)
{
    for (unsigned i = 0; i < sizeof remapped / sizeof *remapped; i++)
        if (remapped[i] == l) return 1;
    return 0;
}

int main(void)
{
    /* The five defaults all collapse onto the user locale. */
    printf("neutral=%#06x user=%#06x system=%#06x custom=%#06x unspec=%#06x\n",
           (unsigned)ConvertDefaultLocale(0x0000),
           (unsigned)ConvertDefaultLocale(0x0400),
           (unsigned)ConvertDefaultLocale(0x0800),
           (unsigned)ConvertDefaultLocale(0x0c00),
           (unsigned)ConvertDefaultLocale(0x1000));

    /* A few concrete ones spelled out, so a failure names something readable before
     * the checksum line. 0x0409 is the one WinMerge actually asks for. */
    printf("en_US=%#06x fr_FR=%#06x en_GB=%#06x ja=%#06x\n",
           (unsigned)ConvertDefaultLocale(0x0409),
           (unsigned)ConvertDefaultLocale(0x040c),
           (unsigned)ConvertDefaultLocale(0x0809),
           (unsigned)ConvertDefaultLocale(0x0411));

    /* Exhaustive sweep of the modelled domain: every non-neutral sublang except the
     * remapped set and the defaults. */
    unsigned long checked = 0, identical = 0, sum = 0;
    for (unsigned sub = 1; sub < 64; sub++) {
        for (unsigned pri = 1; pri < 0x400; pri++) {
            unsigned in = (sub << 10) | pri;
            if (in == 0x0400 || in == 0x0800 || in == 0x0c00 || in == 0x1000) continue;
            if (is_remapped(in)) continue;
            unsigned out = (unsigned)ConvertDefaultLocale(in);
            checked++;
            if (out == in) identical++;
            sum = (sum * 33u + out) & 0xffffffffu;
        }
    }
    printf("swept=%lu identical=%lu checksum=%#010lx\n", checked, identical, sum);

    /* NEUTRAL sublangs, all 1024 of them. This is the sweep that matters most: the
     * mapping is Wine's locale database, and it is embedded in ARET precisely BECAUSE
     * it can be gated exhaustively — a Wine that gained or lost a language shows up
     * here rather than rotting unnoticed. The three groups are counted separately so
     * a failure says which kind moved. */
    unsigned long dflt = 0, thru = 0, other = 0;
    sum = 0;
    for (unsigned id = 1; id < 0x400; id++) {
        unsigned out = (unsigned)ConvertDefaultLocale(id);
        if (out == (id | 0x400)) dflt++;
        else if (out == id) thru++;
        else other++;
        sum = (sum * 33u + out) & 0xffffffffu;
    }
    printf("neutral: default=%lu passthrough=%lu other=%lu checksum=%#010lx\n",
           dflt, thru, other, sum);
    /* The one WinMerge asks for, and the classic non-first-sublang default. */
    printf("neutral en=%#06x zh=%#06x fr=%#06x unassigned93=%#06x\n",
           (unsigned)ConvertDefaultLocale(0x09), (unsigned)ConvertDefaultLocale(0x04),
           (unsigned)ConvertDefaultLocale(0x0c), (unsigned)ConvertDefaultLocale(0x93));
    return 0;
}
