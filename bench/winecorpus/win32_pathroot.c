/* shlwapi path family, wave 1: the ROOT-aware lexical operations.
 *
 * These eight (times A and W) all answer the same underlying question — "where does
 * the root of this path end?" — and they answer it differently from the obvious
 * "split on the last backslash" reading. That is why they are gated together on one
 * shared grid of inputs rather than one fixture each: a mistake in the root rule
 * shows up in several columns at once, and the columns cross-check each other.
 *
 * The grid is chosen to be discriminating, not representative. Every row is a case
 * where a plausible implementation differs from Wine:
 *   - "C:"            a drive with no separator is a root for some of these, not all
 *   - "C:\\"           a bare root must NOT lose its backslash
 *   - "\\\\server\\share"  the UNC root is two components deep, not one
 *   - "\\\\server"       an incomplete UNC has no root at all
 *   - "C:file"        drive-relative: neither absolute nor rooted
 *   - "//server/share" forward slashes are separators here too
 *   - ""              the empty path, where several of these must not write
 *
 * Pointer-returning functions print the OFFSET into the caller's buffer, never the
 * pointer: the address is environmental, the offset is the contract. A NULL return
 * prints -1, which is distinguishable from every real offset.
 *
 * The buffer is pre-filled with a poison byte and the whole tail is dumped after the
 * in-place operations, because "did it write exactly this much" is the part a test
 * that only reads the resulting string cannot see (the lesson from user32_ncm).
 *
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>

/* The one grid, used by every probe below. */
static const char *const CASES_A[] = {
    "", "a", "C:", "C:\\", "C:\\dir", "C:\\dir\\", "C:\\dir\\file.txt",
    "\\\\server\\share", "\\\\server\\share\\", "\\\\server\\share\\dir\\f.txt",
    "\\\\server", "\\\\", "\\", "\\dir\\f", "dir\\f", "..\\f",
    "C:file", "//server/share", "C:/dir/f", "\\\\?\\C:\\x",
};
static const wchar_t *const CASES_W[] = {
    L"", L"a", L"C:", L"C:\\", L"C:\\dir", L"C:\\dir\\", L"C:\\dir\\file.txt",
    L"\\\\server\\share", L"\\\\server\\share\\", L"\\\\server\\share\\dir\\f.txt",
    L"\\\\server", L"\\\\", L"\\", L"\\dir\\f", L"dir\\f", L"..\\f",
    L"C:file", L"//server/share", L"C:/dir/f", L"\\\\?\\C:\\x",
};
#define NCASES ((int)(sizeof(CASES_A) / sizeof(CASES_A[0])))

/* Dump a buffer's raw tail so an over- or under-write is visible, not just the
 * string it happens to leave behind. */
static void dump_a(const char *b, int n)
{
    printf(" raw=");
    for (int i = 0; i < n; i++)
        printf("%02x", (unsigned char)b[i]);
}
static void dump_w(const wchar_t *b, int n)
{
    printf(" raw=");
    for (int i = 0; i < n; i++)
        printf("%04x", (unsigned)b[i]);
}

int main(void)
{
    puts("== predicates ==");
    for (int i = 0; i < NCASES; i++) {
        printf("A [%-28s] unc=%d root=%d rel=%d skip=%d\n", CASES_A[i],
               !!PathIsUNCA(CASES_A[i]), !!PathIsRootA(CASES_A[i]),
               !!PathIsRelativeA(CASES_A[i]),
               PathSkipRootA(CASES_A[i]) ? (int)(PathSkipRootA(CASES_A[i]) - CASES_A[i]) : -1);
    }
    for (int i = 0; i < NCASES; i++) {
        printf("W [%-28ls] unc=%d root=%d rel=%d skip=%d\n", CASES_W[i],
               !!PathIsUNCW(CASES_W[i]), !!PathIsRootW(CASES_W[i]),
               !!PathIsRelativeW(CASES_W[i]),
               PathSkipRootW(CASES_W[i]) ? (int)(PathSkipRootW(CASES_W[i]) - CASES_W[i]) : -1);
    }

    puts("== PathAddBackslash ==");
    for (int i = 0; i < NCASES; i++) {
        char b[64];
        memset(b, 0x7e, sizeof b);
        strcpy(b, CASES_A[i]);
        char *r = PathAddBackslashA(b);
        printf("A [%-28s] ret=%d out=[%s]", CASES_A[i], r ? (int)(r - b) : -1, b);
        dump_a(b, 40);
        puts("");
    }
    for (int i = 0; i < NCASES; i++) {
        wchar_t b[64];
        for (int k = 0; k < 64; k++) b[k] = 0x7e7e;
        wcscpy(b, CASES_W[i]);
        wchar_t *r = PathAddBackslashW(b);
        printf("W [%-28ls] ret=%d out=[%ls]", CASES_W[i], r ? (int)(r - b) : -1, b);
        dump_w(b, 40);
        puts("");
    }
    /* The buffer that cannot take one more character: MAX_PATH is the documented
     * limit and the failure mode (does it write anyway?) is the interesting part. */
    {
        char b[MAX_PATH + 8];
        memset(b, 0x7e, sizeof b);
        memset(b, 'x', MAX_PATH - 1);
        b[MAX_PATH - 1] = 0;
        char *r = PathAddBackslashA(b);
        printf("A [full MAX_PATH-1] ret=%d len=%d tail=%02x%02x%02x\n",
               r ? 1 : -1, (int)strlen(b),
               (unsigned char)b[MAX_PATH - 1], (unsigned char)b[MAX_PATH],
               (unsigned char)b[MAX_PATH + 1]);
    }

    puts("== PathRemoveBackslash ==");
    for (int i = 0; i < NCASES; i++) {
        char b[64];
        memset(b, 0x7e, sizeof b);
        strcpy(b, CASES_A[i]);
        char *r = PathRemoveBackslashA(b);
        printf("A [%-28s] ret=%d out=[%s]", CASES_A[i], r ? (int)(r - b) : -1, b);
        dump_a(b, 40);
        puts("");
    }
    for (int i = 0; i < NCASES; i++) {
        wchar_t b[64];
        for (int k = 0; k < 64; k++) b[k] = 0x7e7e;
        wcscpy(b, CASES_W[i]);
        wchar_t *r = PathRemoveBackslashW(b);
        printf("W [%-28ls] ret=%d out=[%ls]", CASES_W[i], r ? (int)(r - b) : -1, b);
        dump_w(b, 40);
        puts("");
    }

    puts("== PathStripPath ==");
    for (int i = 0; i < NCASES; i++) {
        char b[64];
        memset(b, 0x7e, sizeof b);
        strcpy(b, CASES_A[i]);
        PathStripPathA(b);
        printf("A [%-28s] out=[%s]", CASES_A[i], b);
        dump_a(b, 40);
        puts("");
    }
    for (int i = 0; i < NCASES; i++) {
        wchar_t b[64];
        for (int k = 0; k < 64; k++) b[k] = 0x7e7e;
        wcscpy(b, CASES_W[i]);
        PathStripPathW(b);
        printf("W [%-28ls] out=[%ls]", CASES_W[i], b);
        dump_w(b, 40);
        puts("");
    }

    puts("== PathRemoveFileSpec ==");
    for (int i = 0; i < NCASES; i++) {
        char b[64];
        memset(b, 0x7e, sizeof b);
        strcpy(b, CASES_A[i]);
        BOOL ok = PathRemoveFileSpecA(b);
        printf("A [%-28s] ret=%d out=[%s]", CASES_A[i], !!ok, b);
        dump_a(b, 40);
        puts("");
    }
    for (int i = 0; i < NCASES; i++) {
        wchar_t b[64];
        for (int k = 0; k < 64; k++) b[k] = 0x7e7e;
        wcscpy(b, CASES_W[i]);
        BOOL ok = PathRemoveFileSpecW(b);
        printf("W [%-28ls] ret=%d out=[%ls]", CASES_W[i], !!ok, b);
        dump_w(b, 40);
        puts("");
    }
    return 0;
}
