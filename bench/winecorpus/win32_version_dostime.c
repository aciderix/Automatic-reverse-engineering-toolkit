/* Three high-breadth kernel32 shims measured across the Win95 corpus:
 *   GetVersion (38/41 binaries), DosDateTimeToFileTime (28), RtlMoveMemory (20).
 * GetVersion's raw value is environment-defined (the reported OS version), so we
 * check the *invariant* that is stable across Wine configs and real Windows: the
 * packed GetVersion agrees with GetVersionEx, reports NT, and a plausible major.
 * DosDateTimeToFileTime is a deterministic calendar conversion -> exact FILETIME.
 * RtlMoveMemory is an overlap-safe copy (imported explicitly; windows.h otherwise
 * macro-expands it to memmove and it would never reach the shim). Deterministic
 * output -> checkable bit-for-bit vs Wine. */
#include <stdio.h>
#include <windows.h>

/* windows.h defines RtlMoveMemory as a memmove macro; undo it and import the real
 * kernel32/ntdll export so both engines run their own implementation. */
#undef RtlMoveMemory
__declspec(dllimport) void WINAPI RtlMoveMemory(void *, const void *, SIZE_T);

int main(void) {
    DWORD gv = GetVersion();
    OSVERSIONINFOA ovi;
    ovi.dwOSVersionInfoSize = sizeof(ovi);
    GetVersionExA(&ovi);
    int consistent = (LOBYTE(LOWORD(gv)) == ovi.dwMajorVersion) &&
                     (HIBYTE(LOWORD(gv)) == ovi.dwMinorVersion);
    int nt = (gv & 0x80000000u) ? 0 : 1;
    int major_ge4 = LOBYTE(LOWORD(gv)) >= 4;
    printf("getversion consistent=%d nt=%d major_ge4=%d\n", consistent, nt, major_ge4);

    /* DosDateTimeToFileTime: 2007-03-14 09:26:53 (sec rounded to even by FAT). */
    WORD fatdate = (WORD)(((2007 - 1980) << 9) | (3 << 5) | 14);
    WORD fattime = (WORD)((9 << 11) | (26 << 5) | (53 / 2));
    FILETIME ft;
    ft.dwLowDateTime = ft.dwHighDateTime = 0;
    BOOL ok = DosDateTimeToFileTime(fatdate, fattime, &ft);
    printf("dosdatetime ok=%d ft=%08lx%08lx\n", (int)ok,
           (unsigned long)ft.dwHighDateTime, (unsigned long)ft.dwLowDateTime);

    /* RtlMoveMemory overlap: shift a string right by 3 within one buffer. */
    char buf[24] = "ABCDEFGHIJ";
    RtlMoveMemory(buf + 3, buf, 10);
    buf[13] = 0;
    printf("rtlmove=%s\n", buf);

    return 0;
}
