/* GetTempPath/GetTempFileName guard. The exact temp *path* differs from Windows
 * legitimately (a native dir, not C:\...\Temp\), so we validate the CONTRACT, not
 * the string: GetTempPath returns a non-empty path ending in a separator;
 * GetTempFileName creates a real, unique, writable file in it; a write/read
 * round-trip returns the same bytes. All printed values are booleans/counts —
 * deterministic and identical under Wine. */
#include <windows.h>
#include <stdio.h>

int main(void) {
    char tp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof tp, tp);
    int ends_sep = n > 0 && (tp[n - 1] == '\\' || tp[n - 1] == '/');
    printf("temppath nonempty=%d ends_sep=%d\n", n > 0, ends_sep);

    char fn[MAX_PATH];
    UINT u = GetTempFileNameA(tp, "art", 0, fn);
    printf("tempfile made=%d\n", u != 0);

    int wrote = 0, rd = 0;
    char buf[8] = {0};
    FILE *f = fopen(fn, "wb");
    if (f) { wrote = (fwrite("ABCDE", 1, 5, f) == 5); fclose(f); }
    f = fopen(fn, "rb");
    if (f) { rd = (int)fread(buf, 1, 5, f); fclose(f); }
    printf("roundtrip wrote=%d read=%d content=%s\n", wrote, rd, buf);
    remove(fn);

    /* wide variants: contract only (created + nonzero unique) */
    WCHAR wtp[MAX_PATH];
    DWORD wn = GetTempPathW(MAX_PATH, wtp);
    WCHAR wfn[MAX_PATH];
    UINT wu = GetTempFileNameW(wtp, L"arw", 0, wfn);
    printf("wide temppath=%d tempfile=%d\n", wn > 0, wu != 0);
    _wremove(wfn);
    return 0;
}
