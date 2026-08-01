/* EnumResourceLanguagesW/A — enumerate the language leaves of one PE resource, calling
 * a LIFTED callback per language. WinMerge/MFC calls the W form during startup.
 *
 * Everything asserted here comes from the binary's OWN .rsrc (built from the companion
 * .rc), so unlike the font enumeration there is no environmental component to separate
 * out: the language ids, the callback count and the argument values are all properties
 * of this executable and identical on any machine.
 *
 * The cases that matter are the ones a plausible implementation gets wrong. The type
 * and name reach the callback VERBATIM — the caller's own MAKEINTRESOURCE values, not
 * something re-derived from the tree — so they are printed. A callback returning FALSE
 * stops the walk AND makes the function return FALSE, yet sets NO error: asking to stop
 * is not a failure, and a shim that reported one would look right until a caller
 * checked GetLastError. Type-absent and name-absent are distinct error codes.
 *
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <stdio.h>

static int calls;

static BOOL CALLBACK cb(HMODULE h, LPCWSTR type, LPCWSTR name, WORD lang, LONG_PTR p)
{
    printf("  cb type=%lu name=%lu lang=%#06x lparam=%ld\n",
           (unsigned long)(ULONG_PTR)type, (unsigned long)(ULONG_PTR)name,
           lang, (long)p);
    calls++;
    return TRUE;
}
static BOOL CALLBACK cb_stop(HMODULE h, LPCWSTR type, LPCWSTR name, WORD lang, LONG_PTR p)
{
    printf("  cb_stop lang=%#06x -> FALSE\n", lang);
    calls++;
    return FALSE;
}

static void run(const char *tag, WORD type, WORD name, ENUMRESLANGPROCW f, LONG_PTR lp)
{
    calls = 0;
    SetLastError(0);
    BOOL r = EnumResourceLanguagesW(GetModuleHandleW(NULL),
                                    MAKEINTRESOURCEW(type), MAKEINTRESOURCEW(name), f, lp);
    printf("%-18s r=%d calls=%d err=%lu\n", tag, r, calls, (unsigned long)GetLastError());
}

int main(void)
{
    run("RCDATA/100",   (WORD)(ULONG_PTR)RT_RCDATA, 100, cb, 42);
    run("RCDATA/200",   (WORD)(ULONG_PTR)RT_RCDATA, 200, cb, 0);
    run("STRING/1",     (WORD)(ULONG_PTR)RT_STRING, 1,   cb, 0);
    run("callback stop", (WORD)(ULONG_PTR)RT_RCDATA, 100, cb_stop, 7);
    run("name absent",  (WORD)(ULONG_PTR)RT_RCDATA, 999, cb, 0);
    run("type absent",  (WORD)(ULONG_PTR)RT_BITMAP, 100, cb, 0);

    /* A form is A/W-symmetric for integer resources: same tree, same answers. */
    calls = 0;
    SetLastError(0);
    BOOL ra = EnumResourceLanguagesA(GetModuleHandleW(NULL),
                                     (LPCSTR)RT_RCDATA, MAKEINTRESOURCEA(100),
                                     (ENUMRESLANGPROCA)cb, 5);
    printf("A form           r=%d calls=%d err=%lu\n", ra, calls, (unsigned long)GetLastError());
    return 0;
}
