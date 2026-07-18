/* SystemParametersInfoA — query/set system parameters. Only display-independent GET
 * actions with a deterministic value (verified vs Wine) are modelled, plus the
 * stateful SPI_{GET,SET}SCREENSAVEACTIVE pair. SET actions and unmodelled GETs abort
 * soundly. The work-area rectangle (SPI_GETWORKAREA) is screen-size dependent — ARET
 * returns its 1024x768 screen invariant (doc 72 4.5), which by design does not match a
 * real display, so it is exercised in the implementation but not asserted here. This
 * fixture checks only the values that are the same on every display. Expected identical
 * under Wine and ARET. */
#include <windows.h>
#include <stdio.h>
int main(void) {
    int beep = 0, ssave = 0, drag = 0, border = 0, wheel = 0;
    int r1 = SystemParametersInfoA(SPI_GETBEEP, 0, &beep, 0);
    int r2 = SystemParametersInfoA(SPI_GETSCREENSAVEACTIVE, 0, &ssave, 0);
    int r3 = SystemParametersInfoA(SPI_GETDRAGFULLWINDOWS, 0, &drag, 0);
    int r4 = SystemParametersInfoA(SPI_GETBORDER, 0, &border, 0);
    int r5 = SystemParametersInfoA(0x0068, 0, &wheel, 0); /* SPI_GETWHEELSCROLLLINES */
    printf("r=%d%d%d%d%d beep=%d ss=%d drag=%d border=%d wheel=%d\n",
           r1, r2, r3, r4, r5, beep, ssave, drag, border, wheel);
    /* SPI_SETSCREENSAVEACTIVE is stateful and self-relative (no absolute env value):
     * disable, read back, re-enable, read back. */
    int rs0 = SystemParametersInfoA(SPI_SETSCREENSAVEACTIVE, 0, 0, 0); int a0 = 1;
    SystemParametersInfoA(SPI_GETSCREENSAVEACTIVE, 0, &a0, 0);
    int rs1 = SystemParametersInfoA(SPI_SETSCREENSAVEACTIVE, 1, 0, 0); int a1 = 0;
    SystemParametersInfoA(SPI_GETSCREENSAVEACTIVE, 0, &a1, 0);
    printf("set0=%d after0=%d set1=%d after1=%d\n", rs0, a0, rs1, a1);
    return 0;
}
