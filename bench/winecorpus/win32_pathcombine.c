/* shlwapi path family, wave 2: PathCanonicalize / PathCombine / PathAppend.
 *
 * These three are one increment because they are one implementation: PathAppend
 * defers to PathCombine, which canonicalizes its result. Gating them separately
 * would let a canonicalization bug hide behind an append that happens not to
 * exercise it.
 *
 * PathCanonicalize is the interesting one — it resolves "." and ".." LEXICALLY, so
 * every question is about what it does at the edges rather than in the middle:
 *   - ".." that would climb past the root ("C:\..\a", "\\srv\sh\..\..")
 *   - a trailing "." or ".." with no component after it
 *   - a bare ".." with no root at all ("..\..\a")
 *   - "..." and other runs of dots, which are NOT a parent reference
 *   - an empty path, and a path that is only separators
 * and for PathCombine, the two cases a caller actually hits: an ABSOLUTE second
 * argument (does the first get ignored?) and ALIASING, where dest == dir, which is
 * exactly how PathAppend calls it.
 *
 * Return values matter as much as the strings: PathCombine returns the destination
 * or NULL, and on NULL the question is what it left in the buffer. The buffer is
 * poisoned and its head dumped so "returned NULL and wrote nothing" is
 * distinguishable from "returned NULL after clobbering the destination".
 *
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>

static void dump(const char *b, int n)
{
    printf(" raw=");
    for (int i = 0; i < n; i++) printf("%02x", (unsigned char)b[i]);
}

static void canon(const char *in)
{
    char out[MAX_PATH];
    memset(out, 0x7e, sizeof out);
    BOOL ok = PathCanonicalizeA(out, in);
    printf("canon [%-24s] ok=%d out=[%s]", in, !!ok, out);
    dump(out, 12);
    puts("");
}

static void canonw(const wchar_t *in)
{
    wchar_t out[MAX_PATH];
    for (int i = 0; i < MAX_PATH; i++) out[i] = 0x7e7e;
    BOOL ok = PathCanonicalizeW(out, in);
    printf("canonW[%-24ls] ok=%d out=[%ls]\n", in, !!ok, out);
}

static void comb(const char *dir, const char *file)
{
    char out[MAX_PATH];
    memset(out, 0x7e, sizeof out);
    char *r = PathCombineA(out, dir, file);
    printf("comb  [%-14s]+[%-14s] ret=%s out=[%s]",
           dir ? dir : "(null)", file ? file : "(null)", r ? "buf" : "NULL", r ? out : "");
    dump(out, 12);
    puts("");
}

static void app(const char *path, const char *more)
{
    char b[MAX_PATH];
    memset(b, 0x7e, sizeof b);
    strcpy(b, path);
    BOOL ok = PathAppendA(b, more);
    printf("app   [%-20s]+[%-14s] ok=%d out=[%s]\n", path, more, !!ok, b);
}

int main(void)
{
    puts("== PathCanonicalize ==");
    canon("");
    canon("C:\\a\\b");
    canon("C:\\a\\.\\b");
    canon("C:\\a\\..\\b");
    canon("C:\\a\\b\\..\\..\\c");
    canon("C:\\..\\a");                 /* climbs past the drive root */
    canon("C:\\a\\..");                 /* trailing .. with nothing after */
    canon("C:\\a\\.");                  /* trailing . with nothing after */
    canon("C:\\a\\...\\b");             /* three dots: NOT a parent reference */
    canon("..\\..\\a");                 /* no root to climb from */
    canon("a\\..\\b");
    canon("\\\\srv\\sh\\a\\..\\b");
    canon("\\\\srv\\sh\\..\\..");       /* climbs past the UNC root */
    canon("\\");
    canon("\\\\");
    canon(".");
    canon("..");
    canon("C:\\a\\\\b");                /* doubled separator */
    canon("C:a\\..\\b");               /* drive-RELATIVE: no separator after "C:" */
    canon("C:");
    canon("C:\\.\\a");
    canon(".\\a");
    canon("..\\a");
    canon("a\\.\\b");
    canon("C:\\a\\b\\..");
    canon("C:\\a\\..\\..\\..");
    canon("\\\\srv\\sh\\..");
    canon("\\\\srv\\..");
    canon("\\\\..\\a");
    canon("\\..\\a");
    canon("\\\\srv\\sh\\a\\.\\b");
    canon("C:\\a\\b\\c\\..\\..\\d\\..\\e");
    /* A and W must agree; only a few rows are needed to pin that down. */
    canonw(L"C:\\a\\..\\b");
    canonw(L"C:\\..\\a");
    canonw(L"..\\..\\a");

    puts("== PathCombine ==");
    comb("C:\\dir", "file.txt");
    comb("C:\\dir\\", "file.txt");      /* dir already ends in a separator */
    comb("C:\\dir", "\\file.txt");
    comb("C:\\dir", "C:\\other\\f");    /* absolute second argument */
    comb("C:\\dir", "..\\up");          /* combine canonicalizes */
    comb("C:\\dir", "");
    comb("C:\\dir", NULL);
    comb(NULL, "file.txt");
    comb(NULL, NULL);
    comb("", "file.txt");
    comb("\\\\srv\\sh", "f");
    comb("C:\\", "..\\x");              /* climbs past the root: failure? */
    comb("C:\\dir", "..\\..\\up");       /* climbs past twice */
    comb("C:\\dir", ".");
    comb("C:\\dir", "..");
    comb("\\\\srv\\sh", "..\\..\\x");
    comb("rel", "\\x");                 /* dir RELATIVE but file rooted: which root? */
    comb("C:\\dir", "C:rel");           /* drive-relative file: absolute or not? */
    comb("\\\\srv\\sh\\d", "\\x");     /* rooted file under a UNC dir */
    comb("C:\\dir", "\\\\srv\\sh");      /* UNC file under a drive dir */
    /* Over MAX_PATH: does it refuse, truncate, or overrun? Guessing here would be a
     * silent wrong result either way, so it is measured rather than assumed. */
    {
        char dir[300], file[100], out[MAX_PATH];
        memset(dir, 'd', 200); dir[0] = 'C'; dir[1] = ':'; dir[2] = '\\'; dir[200] = 0;
        memset(file, 'f', 90);  file[90] = 0;
        memset(out, 0x7e, sizeof out);
        char *r = PathCombineA(out, dir, file);
        printf("comb  LONG %d+%d ret=%s outlen=%d head=[%.6s] raw0=%02x\n",
               (int)strlen(dir), (int)strlen(file), r ? "buf" : "NULL",
               r ? (int)strlen(out) : -1, out, (unsigned char)out[0]);
    }

    /* Aliasing: dest == dir is exactly how PathAppend calls PathCombine. */
    {
        char b[MAX_PATH];
        memset(b, 0x7e, sizeof b);
        strcpy(b, "C:\\dir");
        char *r = PathCombineA(b, b, "sub\\f");
        printf("comb  ALIAS dest==dir ret=%s out=[%s]\n", r ? "buf" : "NULL", b);
    }

    /* The wall that motivated this increment is PathAppendW, so W is exercised too. */
    {
        wchar_t out[MAX_PATH], b[MAX_PATH];
        for (int i = 0; i < MAX_PATH; i++) out[i] = 0x7e7e;
        wchar_t *r = PathCombineW(out, L"C:\\dir", L"..\\up");
        printf("combW [C:\\dir]+[..\\up] ret=%s out=[%ls]\n", r ? "buf" : "NULL", out);
        wcscpy(b, L"C:\\dir");
        BOOL ok = PathAppendW(b, L"\\sub\\f");
        printf("appW  [C:\\dir]+[\\sub\\f] ok=%d out=[%ls]\n", !!ok, b);
        wcscpy(b, L"C:\\dir");
        ok = PathAppendW(b, L"\\\\srv\\sh");
        printf("appW  [C:\\dir]+[\\\\srv\\sh] ok=%d out=[%ls]\n", !!ok, b);
    }

    puts("== PathAppend ==");
    app("C:\\dir", "file.txt");
    app("C:\\dir\\", "file.txt");
    app("C:\\dir", "\\file.txt");       /* leading separators stripped */
    app("C:\\dir", "\\\\srv\\sh");      /* unless the tail is itself UNC */
    app("C:\\dir", "..\\up");
    app("C:\\dir", "");
    app("", "file.txt");
    app("C:\\", "x");
    return 0;
}
