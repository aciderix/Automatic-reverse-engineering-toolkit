/* GetFileTitleW (comdlg32) — the file-name (title) component of a path. Contract measured
 * against Wine: returns 0 and writes the title on success; the required size (title+NUL) when
 * the buffer is too small; -1 for a NULL/empty arg, a wildcard char (* [ ]), or a trailing
 * separator (/ \ :). notepad calls it to build its window title from the open file. */
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>

static void probe(const wchar_t *path, int cb) {
    wchar_t buf[64]; buf[0] = 0;
    short r = GetFileTitleW(path, buf, (WORD)cb);
    char nb[128]; int i = 0; for (; buf[i] && i < 63; i++) nb[i] = (char)buf[i]; nb[i] = 0;
    char np[128]; i = 0; for (; path[i] && i < 63; i++) np[i] = (char)path[i]; np[i] = 0;
    printf("\"%s\" cb=%d -> ret=%d title=\"%s\"\n", np, cb, (int)r, nb);
}

int main(void) {
    probe(L"C:\\dir\\sub\\file.txt", 64);   /* -> 0, "file.txt" */
    probe(L"file.txt", 64);                 /* -> 0, "file.txt" (no path) */
    probe(L"C:\\a\\b\\report.log", 5);      /* buffer too small -> required size (11) */
    probe(L"C:\\dir\\", 64);                /* trailing separator -> -1 */
    probe(L"C:\\a\\*.txt", 64);             /* wildcard -> -1 */
    probe(L"", 64);                         /* empty -> -1 */
    probe(L"a/b/c.doc", 64);               /* forward slashes -> 0, "c.doc" */
    printf("done\n");
    return 0;
}
