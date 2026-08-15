/* Small CRT leftovers on the post-lift wall (doc 90): ctime (asctime(localtime),
 * pointer return) and _fpreset. TZ is pinned to UTC so ctime is deterministic across
 * engines. (__fpecode/__pxcptinfoptrs are internal CRT slots with no public prototype
 * — implemented sound but not directly callable from a fixture.) */
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <float.h>

int main(void) {
    _putenv("TZ=UTC");
    _tzset();
    time_t t = 1000000000;                    /* 2001-09-09 01:46:40 UTC */
    printf("ctime=%s", ctime(&t));            /* string already ends in \n */

    _fpreset();
    volatile double d = 7.0;
    d = d / 2.0;
    printf("fp=%.1f\n", d);                   /* FP still works after reset */

    printf("done\n");
    return 0;
}
