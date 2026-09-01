/* msvcrt strtok_s (MS bounds-checked strtok with an explicit context) — ARET
 * routes the import to aret_strtok_s; the Wine oracle loads the genuine msvcrt.
 * Exercises the first call (str != NULL) and continuations (str == NULL) across
 * two INTERLEAVED tokenizations, which is the whole point of the explicit context
 * (a hidden-static strtok could not do this). Deterministic output. */
#include <stdio.h>
#include <string.h>
/* MinGW's <string.h> may not declare strtok_s (C11 Annex K / MS extension). */
extern char *strtok_s(char *, const char *, char **);
int main(void) {
    char a[] = "alpha,beta;;gamma,,delta";
    char b[] = "one two\tthree  four";
    char *ca = NULL, *cb = NULL;
    /* Interleave two independent tokenizations to prove the context is honoured. */
    char *ta = strtok_s(a, ",;", &ca);
    char *tb = strtok_s(b, " \t", &cb);
    int i = 0;
    while (ta || tb) {
        printf("%d a=[%s] b=[%s]\n", i++, ta ? ta : "(nil)", tb ? tb : "(nil)");
        ta = strtok_s(NULL, ",;", &ca);
        tb = strtok_s(NULL, " \t", &cb);
    }
    /* Leading-delimiter and all-delimiter edge cases. */
    char c[] = ";;;x;;y;;;";
    char *cc = NULL;
    for (char *t = strtok_s(c, ";", &cc); t; t = strtok_s(NULL, ";", &cc))
        printf("edge [%s]\n", t);
    printf("done\n");
    return 0;
}
