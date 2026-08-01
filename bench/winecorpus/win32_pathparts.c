/* shlwapi path family, wave 3: EXTENSIONS, COMPONENTS and ROOT COMPARISON.
 *
 * Thirteen functions (times A and W) measured on one shared grid, because they read
 * the same path the same way and a mistake in one shows up in several columns. This
 * wave is deliberately taken as a BLOCK rather than waiting for each to become a
 * runtime wall: the technique is established (poisoned buffer, raw dump, offsets
 * instead of pointers), so the marginal cost of a whole family is one grid.
 *
 * Cases are chosen to discriminate, not to be typical:
 *   - "a.b.c"        which dot is "the" extension
 *   - ".hidden"      a leading dot: extension or name?
 *   - "dir.x\file"   a dot in a DIRECTORY component, not in the file
 *   - "C:"/"C:\"     drive with and without separator (they differ, measured in wave 1)
 *   - "\\srv"/"\\srv\sh"  the two halves of a UNC root
 *   - trailing separators, empty strings, NULL
 *   - "a b c"        where the arguments begin, and what a quoted path does
 *
 * PathCommonPrefix and PathIsPrefix are deliberately ABSENT: 11 measured pairs did
 * not pin their rule down (the returned length sometimes includes the separator and
 * sometimes does not, in a way this grid cannot separate from the root length), so
 * they are left unimplemented — an abort — rather than shipped on a guessed rule.
 *
 * PathIsUNCServer is deliberately ABSENT. Measured, Wine's A entry point answers
 * FALSE for EVERY input while its W entry point answers correctly — the two cannot
 * both be right, and which one matches real Windows is not something this host can
 * decide. Rather than encode a coin flip, that pair is left unimplemented so a call
 * aborts loudly. See the journal entry for the measurement.
 *
 * Pointer results print an OFFSET (-1 for NULL): the address is environmental, the
 * offset is the contract. In-place writers dump the raw buffer, which is what caught
 * a divergence past the terminating NUL in wave 2.
 *
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>

static const char *const G[] = {
    "", "a", "a.b", "a.b.c", ".hidden", "file.", "dir.x\\file", "dir.x\\file.txt",
    "C:", "C:\\", "C:\\dir", "C:\\dir\\", "C:\\dir\\f.txt",
    "\\\\srv", "\\\\srv\\", "\\\\srv\\sh", "\\\\srv\\sh\\", "\\\\srv\\sh\\d\\f.txt",
    "\\", "\\dir\\f", "dir\\f", "a b", "\"a b\" c", "x.exe arg1 arg2",
};
#define N ((int)(sizeof(G) / sizeof(G[0])))

static void dump(const char *b, int n)
{
    printf(" raw=");
    for (int i = 0; i < n; i++) printf("%02x", (unsigned char)b[i]);
}

int main(void)
{
    puts("== pointer probes ==");
    for (int i = 0; i < N; i++) {
        LPSTR nc = PathFindNextComponentA(G[i]);
        LPSTR ar = PathGetArgsA(G[i]);
        LPSTR ex = PathFindExtensionA(G[i]);
        printf("A [%-20s] next=%d args=%d ext=%d drive=%d filespec=%d uncshare=%d\n",
               G[i], nc ? (int)(nc - G[i]) : -1, ar ? (int)(ar - G[i]) : -1,
               ex ? (int)(ex - G[i]) : -1,
               PathGetDriveNumberA(G[i]), !!PathIsFileSpecA(G[i]),
               !!PathIsUNCServerShareA(G[i]));
    }
    {
        wchar_t w[64];
        for (int i = 0; i < N; i++) {
            MultiByteToWideChar(CP_ACP, 0, G[i], -1, w, 64);
            LPWSTR nc = PathFindNextComponentW(w);
            printf("W [%-20s] next=%d args=%d ext=%d drive=%d filespec=%d uncshare=%d\n",
                   G[i], nc ? (int)(nc - w) : -1,
                   PathGetArgsW(w) ? (int)(PathGetArgsW(w) - w) : -1,
                   PathFindExtensionW(w) ? (int)(PathFindExtensionW(w) - w) : -1,
                   PathGetDriveNumberW(w), !!PathIsFileSpecW(w),
                   !!PathIsUNCServerShareW(w));
        }
    }

    puts("== PathAddExtension ==");
    for (int i = 0; i < N; i++) {
        char b[64];
        memset(b, 0x7e, sizeof b); strcpy(b, G[i]);
        BOOL ok = PathAddExtensionA(b, ".ext");
        printf("A [%-20s] ok=%d out=[%s]", G[i], !!ok, b); dump(b, 28); puts("");
    }
    /* An empty extension, and a NULL one: two different requests. */
    {
        char b[64];
        memset(b, 0x7e, sizeof b); strcpy(b, "a");
        printf("A [a]+\"\"   ok=%d out=[%s]\n", !!PathAddExtensionA(b, ""), b);
        memset(b, 0x7e, sizeof b); strcpy(b, "a");
        printf("A [a]+NULL ok=%d out=[%s]\n", !!PathAddExtensionA(b, NULL), b);
    }

    puts("== PathRemoveExtension ==");
    for (int i = 0; i < N; i++) {
        char b[64];
        memset(b, 0x7e, sizeof b); strcpy(b, G[i]);
        PathRemoveExtensionA(b);
        printf("A [%-20s] out=[%s]", G[i], b); dump(b, 28); puts("");
    }

    puts("== PathRenameExtension ==");
    for (int i = 0; i < N; i++) {
        char b[64];
        memset(b, 0x7e, sizeof b); strcpy(b, G[i]);
        BOOL ok = PathRenameExtensionA(b, ".new");
        printf("A [%-20s] ok=%d out=[%s]", G[i], !!ok, b); dump(b, 28); puts("");
    }

    puts("== PathStripToRoot ==");
    for (int i = 0; i < N; i++) {
        char b[64];
        memset(b, 0x7e, sizeof b); strcpy(b, G[i]);
        BOOL ok = PathStripToRootA(b);
        printf("A [%-20s] ok=%d out=[%s]", G[i], !!ok, b); dump(b, 28); puts("");
    }

    puts("== PathIsSameRoot ==");
    {
        static const char *const pairs[][2] = {
            {"C:\\a", "C:\\b"}, {"C:\\a", "D:\\a"}, {"C:\\a", "a"}, {"C:\\a", "c:\\b"},
            {"\\\\srv\\sh\\a", "\\\\srv\\sh\\b"}, {"\\\\srv\\sh\\a", "\\\\srv\\o\\b"},
            {"C:\\dir", "C:\\dir\\f"}, {"C:\\dir", "C:\\dirx\\f"},
            {"C:\\a\\b", "C:\\a\\b"}, {"", ""}, {"C:\\", "C:\\"}, {"a", "b"},
        };
        for (int i = 0; i < (int)(sizeof(pairs) / sizeof(pairs[0])); i++)
            printf("[%-14s][%-14s] sameroot=%d\n", pairs[i][0], pairs[i][1],
                   !!PathIsSameRootA(pairs[i][0], pairs[i][1]));
    }
    return 0;
}
