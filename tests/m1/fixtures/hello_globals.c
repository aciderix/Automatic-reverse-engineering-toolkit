/* M2 fixture: global strings living in .rdata, referenced by their absolute
   virtual address (not built on the stack). Exercises the Memory Layout Mapper:
   the recompiled binary must place these sections back at their original VAs so
   the absolute pointers resolve. Transpiled and recompiled natively for Linux. */
#include <windows.h>

static const char LINE1[] = "M2: first global string in .rdata\n";
static const char LINE2[] = "M2: second global, mapped at its original VA\n";

void __stdcall mainCRTStartup(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    WriteFile(h, LINE1, sizeof(LINE1) - 1, &written, 0);
    WriteFile(h, LINE2, sizeof(LINE2) - 1, &written, 0);
    ExitProcess(0);
}
