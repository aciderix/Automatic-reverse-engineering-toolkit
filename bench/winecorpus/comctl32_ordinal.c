/* Import by ORDINAL. COMCTL32.dll exports InitCommonControls as ordinal 17 (no
 * name), the Win95-era ABI many old apps link against. The importer's table holds a
 * raw 0x80000000|17 = 0x80000011 with no name, so ARET must map (comctl32, 17) to the
 * export name to route the call to its shim — otherwise the indirect call aborts on
 * the opaque address (the real wall hit by itiem95.exe on the Chip 1997 CD). The
 * companion comctl32_ordinal.def forces the ordinal import (dlltool import lib linked
 * first); a plain -lcomctl32 would import by name instead. Expected (Wine and ARET):
 * ok. */
#include <windows.h>
#include <stdio.h>
__declspec(dllimport) void __stdcall InitCommonControls(void);
int main(void) {
    InitCommonControls();      /* ordinal-17 import; must resolve, not abort */
    printf("ok\n");
    return 0;
}
