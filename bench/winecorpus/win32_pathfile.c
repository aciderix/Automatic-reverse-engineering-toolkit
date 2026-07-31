/* PathFindFileNameA/W (shlwapi) and GetProfileIntW (kernel32). WinMerge/MFC calls
 * PathFindFileNameW and GetProfileIntW during startup.
 *
 * PathFindFileName returns a pointer INTO the caller's buffer, so the fixture prints
 * the OFFSET rather than the pointer: the address is environmental, the offset is the
 * contract. The cases chosen are the ones where the obvious implementation — "return
 * whatever follows the last separator" — gives a different answer than Wine: a path
 * ending in a separator, a bare root, and a UNC prefix all keep more than that rule
 * would leave. A separator only counts when the next character exists and is not itself
 * a slash, and these cases are what pin that down.
 *
 * GetProfileIntW is queried only with sections proven ABSENT. Wine's own win.ini is
 * populated — intl/iCountry answers 1 rather than the caller's default — so reading a
 * real section would compare Wine's environment against our (empty) model and diverge
 * for reasons that have nothing to do with the shim. Contract, not data. What is
 * asserted is the part that is deterministic: a missing key yields the caller's
 * default, including 0, while a NULL app name yields 0 rather than the default — the
 * two NULL arguments do not behave alike.
 *
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>

static void pw(const wchar_t *in)
{
    wchar_t buf[64];
    wcscpy(buf, in);
    wchar_t *r = PathFindFileNameW(buf);
    printf("W [%-22ls] off=%d tail=[%ls]\n", in, (int)(r - buf), r);
}
static void pa(const char *in)
{
    char buf[64];
    strcpy(buf, in);
    char *r = PathFindFileNameA(buf);
    printf("A [%-22s] off=%d tail=[%s]\n", in, (int)(r - buf), r);
}

int main(void)
{
    pw(L"C:\\dir\\sub\\file.txt");
    pw(L"file.txt");
    pw(L"C:\\dir\\");          /* trailing separator: keeps "dir\" */
    pw(L"C:\\");               /* bare root: keeps the whole string */
    pw(L"");
    pw(L"a/b/c.txt");          /* forward slashes count */
    pw(L"\\\\srv\\share\\f.dat"); /* UNC: the leading \\ does not count */
    pw(L"C:file.txt");         /* a bare ':' counts */
    pw(L"dir\\");
    pw(L"..\\x");
    pw(L"a\\\\b");             /* separator followed by a separator */
    pa("C:\\dir\\sub\\file.txt");
    pa("C:\\dir\\");
    pa("C:\\");
    pa("a/b/c.txt");
    printf("W NULL -> %d\n", PathFindFileNameW(NULL) == NULL);
    printf("A NULL -> %d\n", PathFindFileNameA(NULL) == NULL);

    /* Absent sections only — see the header note on why intl/windows are excluded. */
    printf("prof absent def=1234 -> %u\n", GetProfileIntW(L"AretNoSuchApp", L"NoSuchKey", 1234));
    printf("prof absent def=0    -> %u\n", GetProfileIntW(L"AretNoSuchApp", L"NoSuchKey", 0));
    printf("prof app NULL        -> %u\n", GetProfileIntW(NULL, L"k", 7));
    printf("prof key NULL        -> %u\n", GetProfileIntW(L"AretNoSuchApp", NULL, 7));
    return 0;
}
