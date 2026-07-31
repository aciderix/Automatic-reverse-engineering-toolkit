/* SystemParametersInfoW — the "UI effects" family (0x1000-0x1042): the per-effect BOOLs
 * every modern shell and framework queries at startup. MFC (WinMerge) stops on
 * SPI_GETMENUANIMATION (0x1002), which is what motivated modelling the whole family
 * rather than that one action.
 *
 * Each query runs against a POISONED buffer (0xAB) and the raw bytes are printed, not
 * just the value. That is the point of the fixture: it proves each action writes exactly
 * ONE 32-bit BOOL and leaves the rest of the buffer untouched — a test that only read
 * back an int would pass just as happily on a shim that wrote 8 bytes, or none.
 *
 * The values are NOT uniform across the family and are not derivable from each other or
 * from anything else, so each was measured. Three actions are measured as REJECTED by
 * Wine: they return FALSE, set ERROR_INVALID_SPI_VALUE, and leave pvParam entirely
 * untouched — the poison survives, which is exactly what this fixture checks.
 *
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <stdio.h>

static const struct { unsigned action; const char *name; } T[] = {
    { 0x1000, "ACTIVEWINDOWTRACKING"   }, { 0x1002, "MENUANIMATION"     },
    { 0x1004, "COMBOBOXANIMATION"      }, { 0x1006, "LISTBOXSMOOTH"     },
    { 0x1008, "GRADIENTCAPTIONS"       }, { 0x100A, "KEYBOARDCUES"      },
    { 0x100C, "ACTIVEWNDTRKZORDER"     }, { 0x100E, "HOTTRACKING"       },
    { 0x1012, "MENUFADE"               }, { 0x1014, "SELECTIONFADE"     },
    { 0x1016, "TOOLTIPANIMATION"       }, { 0x1018, "TOOLTIPFADE"       },
    { 0x101A, "CURSORSHADOW"           }, { 0x1022, "FLATMENU"          },
    { 0x1024, "DROPSHADOW"             }, { 0x1042, "CLIENTAREAANIM"    },
    /* rejected by Wine */
    { 0x0042, "SHOWSOUNDS"             }, { 0x102A, "UIEFFECTS"         },
    { 0x1082, "MOUSEVANISH"            },
};

int main(void)
{
    for (unsigned i = 0; i < sizeof T / sizeof *T; i++) {
        unsigned char buf[8];
        memset(buf, 0xAB, sizeof buf);
        SetLastError(0);
        int r = SystemParametersInfoW(T[i].action, 0, buf, 0);
        /* Print the raw bytes: which ones moved is as much the contract as the value. */
        printf("%-22s %#06x r=%d err=%lu raw=%02x%02x%02x%02x%02x%02x%02x%02x\n",
               T[i].name, T[i].action, r, (unsigned long)GetLastError(),
               buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
    }
    /* A rejected action must not touch the buffer at all — asserted explicitly so a shim
     * that zero-filled on failure (a plausible and wrong implementation) would be caught. */
    unsigned char p[8];
    memset(p, 0xAB, sizeof p);
    SystemParametersInfoW(0x102A, 0, p, 0);
    int intact = 1;
    for (unsigned i = 0; i < sizeof p; i++) if (p[i] != 0xAB) intact = 0;
    printf("uieffects_buffer_intact=%d\n", intact);
    /* A/W parity: the family is display-independent, so both entry points agree. */
    int a = -1, w = -1;
    SystemParametersInfoA(0x1002, 0, &a, 0);
    SystemParametersInfoW(0x1002, 0, &w, 0);
    printf("menuanimation A=%d W=%d same=%d\n", a, w, a == w);
    return 0;
}
