/* Window station / desktop — the "which desktop am I on" family a framework asks at
 * startup (MFC/WinMerge's wall right after shlwapi's Str*).
 *
 * HANDLES are addresses, so this prints RELATIONS (null / same / different), never
 * handle values: a value differs between two honest runs and would make the gate
 * flap. What is a contract here is that the two handles are stable singletons and
 * that they are distinct from each other.
 *
 * Every GetUserObjectInformation call writes into a POISONED buffer whose raw bytes
 * are dumped, because the interesting facts are all about what is NOT written:
 *   - UOI_FLAGS fills a 12-byte USEROBJECTFLAGS but touches only the THIRD dword —
 *     fInherit and fReserved keep the poison. A shim that zero-filled the struct
 *     would pass any test that reads dwFlags alone.
 *   - a too-small buffer, an unsupported index, and a bad handle each leave the
 *     buffer completely intact, and they report DIFFERENT things in
 *     *lpnLengthNeeded (a size / zero / zero) and different last errors
 *     (122 / 87 / 6).
 *   - ⚠️ on the A path the FAILURE branch reports the WIDE byte count (16 for
 *     "Default") while SUCCESS reports the narrow one (8). That is reproduced
 *     verbatim because Wine is the gate, and it is QUEUED FOR THE WINDOWS ORACLE
 *     (bench/winoracle/win32_desktopdisputed.c) — a required size that depends on
 *     whether you succeeded looks like Wine's A wrapper leaking its W call.
 *
 * The last error is cleared before every call: a probe that reads global state and
 * does not reset it measures the previous call's residue.
 *
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static void dump(const char *what, BOOL ok, DWORD err, DWORD need,
                 const unsigned char *b, int n)
{
    printf("%-24s ok=%d err=%lu need=%lu raw=", what, ok,
           (unsigned long)err, (unsigned long)need);
    for (int i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}

int main(void)
{
    SetLastError(0);
    HDESK d1 = GetThreadDesktop(GetCurrentThreadId());
    printf("GetThreadDesktop null=%d err=%lu\n", d1 == NULL, (unsigned long)GetLastError());
    SetLastError(0);
    HDESK d2 = GetThreadDesktop(GetCurrentThreadId());
    printf("stable singleton same=%d\n", d1 == d2);
    SetLastError(0);
    HDESK d0 = GetThreadDesktop(0);
    printf("tid 0     null=%d err=%lu\n", d0 == NULL, (unsigned long)GetLastError());
    SetLastError(0);
    HDESK db = GetThreadDesktop(0x7fffffff);
    printf("tid bogus null=%d err=%lu\n", db == NULL, (unsigned long)GetLastError());

    SetLastError(0);
    HWINSTA w1 = GetProcessWindowStation();
    printf("GetProcessWindowStation null=%d err=%lu\n", w1 == NULL, (unsigned long)GetLastError());
    HWINSTA w2 = GetProcessWindowStation();
    printf("stable singleton same=%d distinct-from-desktop=%d\n",
           w1 == w2, (void *)w1 != (void *)d1);

    unsigned char buf[64];
    DWORD need;
    char nm[48];
    for (int idx = 1; idx <= 5; idx++) {
        memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
        BOOL ok = GetUserObjectInformationA(d1, idx, buf, sizeof buf, &need);
        sprintf(nm, "A desk idx=%d", idx);   dump(nm, ok, GetLastError(), need, buf, 16);
        memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
        ok = GetUserObjectInformationA(w1, idx, buf, sizeof buf, &need);
        sprintf(nm, "A winsta idx=%d", idx); dump(nm, ok, GetLastError(), need, buf, 16);
    }
    for (int idx = 2; idx <= 3; idx++) {
        memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
        BOOL ok = GetUserObjectInformationW(d1, idx, buf, sizeof buf, &need);
        sprintf(nm, "W desk idx=%d", idx);   dump(nm, ok, GetLastError(), need, buf, 20);
        memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
        ok = GetUserObjectInformationW(w1, idx, buf, sizeof buf, &need);
        sprintf(nm, "W winsta idx=%d", idx); dump(nm, ok, GetLastError(), need, buf, 30);
    }

    /* Too small by one, exactly big enough, and a size query. */
    memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
    dump("A name len=2", GetUserObjectInformationA(d1, UOI_NAME, buf, 2, &need),
         GetLastError(), need, buf, 12);
    memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
    dump("A name len=7", GetUserObjectInformationA(d1, UOI_NAME, buf, 7, &need),
         GetLastError(), need, buf, 12);
    memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
    dump("A name len=8", GetUserObjectInformationA(d1, UOI_NAME, buf, 8, &need),
         GetLastError(), need, buf, 12);
    need = 0; SetLastError(0);
    printf("A name size-query ok=%d err=%lu need=%lu\n",
           GetUserObjectInformationA(d1, UOI_NAME, NULL, 0, &need),
           (unsigned long)GetLastError(), (unsigned long)need);

    memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
    dump("bad handle", GetUserObjectInformationA((HANDLE)(ULONG_PTR)0x1234, UOI_NAME,
         buf, sizeof buf, &need), GetLastError(), need, buf, 12);

    /* lpnLengthNeeded may be NULL on the success path. */
    memset(buf, 0xAA, sizeof buf); SetLastError(0);
    BOOL ok = GetUserObjectInformationA(d1, UOI_NAME, buf, sizeof buf, NULL);
    printf("null-needed ok=%d err=%lu name=\"%s\"\n", ok,
           (unsigned long)GetLastError(), ok ? (char *)buf : "");
    return 0;
}
