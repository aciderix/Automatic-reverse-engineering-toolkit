/* Un vrai programme C compilé NORMALEMENT (démarrage CRT mingw complet :
   crt2.o -> __mingw32 init -> _initterm (constructeurs) -> __getmainargs ->
   main(argc, argv)). Pas de mainCRTStartup freestanding. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int counter = 0;
__attribute__((constructor)) static void ctor(void) { counter = 100; }

int main(int argc, char **argv) {
    printf("REALCRT: argc=%d ctor=%d\n", argc, counter);
    char *p = malloc(32);
    strcpy(p, "real crt heap");
    printf("REALCRT: heap=%s len=%zu\n", p, strlen(p));
    free(p);
    return 0;
}
