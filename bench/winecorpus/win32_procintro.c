/* HLE family: process introspection (psapi K32* + kernel32), part of the 2nd-tier OS
 * wall measured (doc 90, 2026-08-16) once the C++ runtime auto-lifts. Exercises
 * GetProcessMemoryInfo, EnumProcessModules, FlushInstructionCache, GetLargePageMinimum.
 * Prints only DETERMINISTIC facts (success bools + invariants) so ARET and Wine match
 * bit-for-bit; the raw memory counters, module count and page size are env-dependent
 * and never printed. */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>

int main(void) {
    HANDLE self = GetCurrentProcess();

    PROCESS_MEMORY_COUNTERS pmc;
    pmc.cb = sizeof pmc;
    BOOL mr = GetProcessMemoryInfo(self, &pmc, sizeof pmc);
    printf("meminfo=%d inv=%d\n", mr != 0,
           mr && pmc.cb == sizeof pmc && pmc.PeakWorkingSetSize >= pmc.WorkingSetSize);

    HMODULE mods[256]; DWORD needed = 0;
    BOOL er = EnumProcessModules(self, mods, sizeof mods, &needed);
    printf("enummods=%d inv=%d\n", er != 0, er && needed >= sizeof(HMODULE) && mods[0] != NULL);

    printf("flushic=%d\n", FlushInstructionCache(self, NULL, 0) != 0);

    SIZE_T lpm = GetLargePageMinimum();
    printf("largepage=%d\n", lpm == 0 || lpm == 0x200000);

    printf("done\n");
    return 0;
}
