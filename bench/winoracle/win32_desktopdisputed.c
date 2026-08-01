/* GetUserObjectInformation — the cell where Wine's A wrapper looks like it leaks.
 *
 * The window-station/desktop family was measured against Wine and shipped
 * (winecorpus/user32_desktop.c). One result there cannot be settled by Wine,
 * because Wine is the suspect:
 *
 *   On the ANSI path, SUCCESS reports the narrow byte count in *lpnLengthNeeded
 *   (8 for "Default") while FAILURE reports the WIDE one (16). A required size that
 *   depends on whether the call succeeded is not a contract anyone would design; it
 *   is what an A wrapper does when it forwards to the W entry point and only
 *   converts on the way out. If Windows reports 8 in both cases, then every caller
 *   that allocates from a failed size query allocates twice what it needs under us —
 *   harmless — but a caller that COMPARES the two values takes a different branch.
 *
 * Also probed, because they are cheap once the program exists and each would change
 * what we write: whether UOI_FLAGS really leaves fInherit/fReserved untouched (the
 * buffer is poisoned so the raw bytes prove it), and whether an unsupported index
 * and a bad handle really differ in both last error and *lpnLengthNeeded.
 *
 * Handles are never printed — only relations. The last error is cleared before each
 * call so nothing measures the previous one's residue. */
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
    HDESK   d = GetThreadDesktop(GetCurrentThreadId());
    HWINSTA w = GetProcessWindowStation();
    printf("desktop null=%d winsta null=%d distinct=%d\n",
           d == NULL, w == NULL, (void *)d != (void *)w);

    unsigned char buf[64];
    DWORD need;

    /* THE disputed cell: the same query, succeeding and failing. */
    memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
    dump("A UOI_NAME len=64", GetUserObjectInformationA(d, UOI_NAME, buf, sizeof buf, &need),
         GetLastError(), need, buf, 12);
    memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
    dump("A UOI_NAME len=2",  GetUserObjectInformationA(d, UOI_NAME, buf, 2, &need),
         GetLastError(), need, buf, 12);
    need = 0; SetLastError(0);
    printf("%-24s ok=%d err=%lu need=%lu\n", "A UOI_NAME query",
           GetUserObjectInformationA(d, UOI_NAME, NULL, 0, &need),
           (unsigned long)GetLastError(), (unsigned long)need);
    memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
    dump("W UOI_NAME len=64", GetUserObjectInformationW(d, UOI_NAME, buf, sizeof buf, &need),
         GetLastError(), need, buf, 20);

    /* UOI_FLAGS: is the whole USEROBJECTFLAGS written, or only dwFlags? */
    memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
    dump("A UOI_FLAGS desk",   GetUserObjectInformationA(d, UOI_FLAGS, buf, sizeof buf, &need),
         GetLastError(), need, buf, 12);
    memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
    dump("A UOI_FLAGS winsta", GetUserObjectInformationA(w, UOI_FLAGS, buf, sizeof buf, &need),
         GetLastError(), need, buf, 12);

    /* Object type strings, whose lengths are not a constant. */
    memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
    dump("A UOI_TYPE desk",   GetUserObjectInformationA(d, UOI_TYPE, buf, sizeof buf, &need),
         GetLastError(), need, buf, 16);
    memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
    dump("A UOI_TYPE winsta", GetUserObjectInformationA(w, UOI_TYPE, buf, sizeof buf, &need),
         GetLastError(), need, buf, 16);

    /* Unsupported index vs bad handle: two different failures, or one? */
    memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
    dump("A idx 4 (USER_SID)", GetUserObjectInformationA(d, UOI_USER_SID, buf, sizeof buf, &need),
         GetLastError(), need, buf, 12);
    memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
    dump("A idx 5", GetUserObjectInformationA(d, 5, buf, sizeof buf, &need),
         GetLastError(), need, buf, 12);
    memset(buf, 0xAA, sizeof buf); need = 0; SetLastError(0);
    dump("A bad handle", GetUserObjectInformationA((HANDLE)(ULONG_PTR)0x1234, UOI_NAME,
         buf, sizeof buf, &need), GetLastError(), need, buf, 12);

    /* And whether a thread id that is not ours is really rejected. */
    SetLastError(0);
    printf("GetThreadDesktop(0)     null=%d err=%lu\n",
           GetThreadDesktop(0) == NULL, (unsigned long)GetLastError());
    SetLastError(0);
    printf("GetThreadDesktop(bogus) null=%d err=%lu\n",
           GetThreadDesktop(0x7fffffff) == NULL, (unsigned long)GetLastError());
    return 0;
}
