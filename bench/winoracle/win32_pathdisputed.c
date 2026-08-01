/* The questions Wine cannot answer — for a REAL Windows oracle (GitHub Actions).
 *
 * Two shlwapi functions were left unimplemented in wave 3 because Wine's answers do
 * not determine the contract, and this is the probe that settles them. It lives in
 * `bench/winoracle/` rather than `bench/winecorpus/` because it is NOT a winediff
 * fixture: winediff compares ARET against Wine, and the whole point here is that
 * Wine is the thing under suspicion.
 *
 * 1. PathIsUNCServer. Measured under Wine: the A entry point answers FALSE for
 *    EVERY input while the W entry point answers correctly. They cannot both be
 *    right. Windows decides which — and if Windows agrees with W, Wine's A is a bug
 *    we must NOT reproduce.
 *
 * 2. PathCommonPrefix / PathIsPrefix. Eleven pairs measured under Wine left the rule
 *    ambiguous: the returned length sometimes includes the trailing separator and
 *    sometimes does not, in a way those pairs cannot separate from "the length is
 *    the root length". The pairs below are chosen to break that tie — cases where
 *    the two candidate rules give DIFFERENT numbers, which the earlier grid lacked.
 *
 * Output is deliberately plain and stable so it can be diffed against the same
 * program's output under Wine.
 *
 * NOTE for whoever adds a probe here: the workflow triggers on `paths:`, so a commit
 * that only fixes something OUTSIDE those paths will not re-run it. That is how the
 * first green run had to be forced. */
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>

int main(void)
{
    puts("== PathIsUNCServer (A vs W must agree) ==");
    {
        static const char *const g[] = {
            "", "a", "C:\\x", "\\", "\\\\", "\\\\a", "\\\\srv", "\\\\srv\\",
            "\\\\srv\\sh", "\\\\srv\\sh\\", "\\\\?\\C:\\x",
        };
        wchar_t w[64];
        for (int i = 0; i < (int)(sizeof(g) / sizeof(g[0])); i++) {
            MultiByteToWideChar(CP_ACP, 0, g[i], -1, w, 64);
            printf("[%-14s] serverA=%d serverW=%d shareA=%d shareW=%d\n", g[i],
                   !!PathIsUNCServerA(g[i]), !!PathIsUNCServerW(w),
                   !!PathIsUNCServerShareA(g[i]), !!PathIsUNCServerShareW(w));
        }
    }

    puts("== PathCommonPrefix / PathIsPrefix (tie-breakers) ==");
    {
        /* Each pair is picked so that "include the separator" and "stop at the root"
         * predict different lengths. The earlier grid only had pairs where the two
         * rules happened to coincide, which is why it proved nothing. */
        static const char *const pairs[][2] = {
            {"C:\\a\\b", "C:\\a\\c"},          /* boundary after "C:\a": 4 or 5? */
            {"C:\\a\\b\\c", "C:\\a\\b\\d"},    /* boundary after "C:\a\b": 6 or 7? */
            {"C:\\aa\\b", "C:\\ab\\b"},        /* differ INSIDE a component */
            {"C:\\a", "C:\\ab"},               /* one is a strict character prefix */
            {"C:\\a\\", "C:\\a\\b"},           /* one ends ON the separator */
            {"\\\\s\\h\\a", "\\\\s\\h\\b"},    /* UNC, boundary just past the root */
            {"\\\\s\\h", "\\\\s\\i"},          /* UNC, differ in the share itself */
            {"a\\b", "a\\c"},                  /* relative, no root at all */
            {"a", "a"},                        /* identical, no separator */
            {"C:\\", "C:\\a"},
            {"C:\\a", "C:\\a"},                /* identical with a root */
        };
        for (int i = 0; i < (int)(sizeof(pairs) / sizeof(pairs[0])); i++) {
            char cp[64];
            memset(cp, 0x7e, sizeof cp);
            int n = PathCommonPrefixA(pairs[i][0], pairs[i][1], cp);
            cp[n < 60 ? n : 60] = 0;
            printf("[%-10s][%-10s] common=%d=[%s] prefixAB=%d prefixBA=%d\n",
                   pairs[i][0], pairs[i][1], n, cp,
                   !!PathIsPrefixA(pairs[i][0], pairs[i][1]),
                   !!PathIsPrefixA(pairs[i][1], pairs[i][0]));
        }
        printf("common NULL-out = %d\n", PathCommonPrefixA("C:\\a\\b", "C:\\a\\c", NULL));
    }
    return 0;
}
