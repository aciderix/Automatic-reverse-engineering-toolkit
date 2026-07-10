/* Three small kernel32 shims flagged by the Win95 corpus:
 *   GetExitCodeProcess (23 binaries), GetDiskFreeSpaceA (25), SetFileAttributesA (29).
 * Environment-dependent raw values (disk sizes) are checked by *invariant* rather
 * than exact bytes, so the output is stable across Wine configs and real Windows;
 * GetExitCodeProcess on the current process is the deterministic STILL_ACTIVE, and
 * SetFileAttributes round-trips through GetFileAttributes (READONLY <-> write bit).
 * All output is deterministic -> checkable bit-for-bit vs Wine. */
#include <stdio.h>
#include <windows.h>

int main(void) {
    /* GetExitCodeProcess on the current process = STILL_ACTIVE (259). */
    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(GetCurrentProcess(), &code);
    printf("exitcode ok=%d still_active=%d\n", (int)ok, code == STILL_ACTIVE);

    /* GetDiskFreeSpaceA: check structure (stable), not host-specific sizes. */
    DWORD spc = 0, bps = 0, freec = 0, totalc = 0;
    BOOL dok = GetDiskFreeSpaceA(NULL, &spc, &bps, &freec, &totalc);
    int bps_pow2 = bps && ((bps & (bps - 1)) == 0);
    printf("disk ok=%d bps_pow2=%d spc_ge1=%d total_ge_free=%d nonzero=%d\n",
           (int)dok, bps_pow2, spc >= 1, totalc >= freec, (totalc != 0));

    /* SetFileAttributesA round-trip: READONLY set then cleared. */
    const char *fn = "aret_attr_test.tmp";
    FILE *f = fopen(fn, "w");
    if (f) { fputs("x", f); fclose(f); }
    SetFileAttributesA(fn, FILE_ATTRIBUTE_READONLY);
    int ro1 = (GetFileAttributesA(fn) & FILE_ATTRIBUTE_READONLY) != 0;
    SetFileAttributesA(fn, FILE_ATTRIBUTE_NORMAL);
    int ro2 = (GetFileAttributesA(fn) & FILE_ATTRIBUTE_READONLY) != 0;
    printf("setattr readonly_after_set=%d readonly_after_clear=%d\n", ro1, ro2);
    remove(fn);
    return 0;
}
