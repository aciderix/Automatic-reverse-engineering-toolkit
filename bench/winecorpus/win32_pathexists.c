/* PathFileExistsA/W and PathIsDirectoryA/W (shlwapi) — the two path predicates that
 * actually touch the filesystem, as opposed to the lexical family next door.
 *
 * The fixture CREATES what it queries (a file and a directory in the working
 * directory) so both engines are asked about the same objects rather than about
 * whatever happens to exist on the host. Nothing pre-existing is probed except the
 * two universal entries "." and "..".
 *
 * What is worth measuring here is not "does an existing file exist" — it is the
 * edges, where a plausible implementation built on stat() diverges:
 *   - a DIRECTORY passed to PathFileExists (does a file-existence test say yes?)
 *   - a trailing separator on a file, and on a directory
 *   - the empty path and NULL
 *   - a WILDCARD, which stat() cannot resolve but which is a legal string
 *   - what PathIsDirectory RETURNS (a bare 1, or FILE_ATTRIBUTE_DIRECTORY?)
 *   - the last error left behind, which callers branch on
 * Each probe resets the last error first so no reading is contaminated by the
 * previous call.
 *
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <direct.h>

static void pfe(const char *p)
{
    SetLastError(0);
    BOOL e = PathFileExistsA(p);
    DWORD err = GetLastError();
    SetLastError(0);
    DWORD d = (DWORD)PathIsDirectoryA(p);
    DWORD err2 = GetLastError();
    printf("A [%-16s] exists=%d err=%lu | isdir=0x%lx err=%lu\n",
           p ? p : "(null)", !!e, (unsigned long)err, (unsigned long)d,
           (unsigned long)err2);
}

static void pfew(const wchar_t *p)
{
    SetLastError(0);
    BOOL e = PathFileExistsW(p);
    DWORD err = GetLastError();
    SetLastError(0);
    DWORD d = (DWORD)PathIsDirectoryW(p);
    printf("W [%-16ls] exists=%d err=%lu | isdir=0x%lx\n",
           p ? p : L"(null)", !!e, (unsigned long)err, (unsigned long)d);
}

int main(void)
{
    /* Build the objects under test, so the answers do not depend on the host. */
    FILE *f = fopen("pfe_file.txt", "wb");
    if (f) { fputs("x", f); fclose(f); }
    _mkdir("pfe_dir");

    pfe("pfe_file.txt");
    pfe("pfe_dir");
    pfe("pfe_dir\\");          /* trailing separator on a directory */
    pfe("pfe_file.txt\\");     /* trailing separator on a FILE */
    pfe("pfe_missing.txt");
    pfe("pfe_dir\\nope");
    pfe(".");
    pfe("..");
    pfe("");
    pfe(NULL);
    pfe("pfe_*.txt");          /* a wildcard: a legal string stat() cannot resolve */
    pfe("pfe_file.txt\\sub");  /* a component under a non-directory */

    pfew(L"pfe_file.txt");
    pfew(L"pfe_dir");
    pfew(L"pfe_missing.txt");
    pfew(L"");

    remove("pfe_file.txt");
    _rmdir("pfe_dir");
    /* After removal the same names must flip, which proves the probes read the
     * filesystem rather than a cache or a constant. */
    pfe("pfe_file.txt");
    pfe("pfe_dir");
    return 0;
}
