/* CRT + Win32 shims enabling the libglib lift (doc 82): SetEnvironmentVariableW/
 * ExpandEnvironmentStringsW, CommandLineToArgvW (Windows quoting rules), _getdrive,
 * _ui64toa_s, wctomb, _kbhit. Deterministic. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <direct.h>
#include <conio.h>

int main(void) {
    SetEnvironmentVariableW(L"ARET_TEST", L"hello");
    wchar_t eb[128];
    ExpandEnvironmentStringsW(L"[%ARET_TEST%]", eb, 128);
    printf("expand=%ls\n", eb);                       /* [hello] */

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(L"app.exe \"hello world\" foo\\bar", &argc);
    printf("argc=%d\n", argc);                         /* 3 */
    for (int i = 0; i < argc; i++) printf("argv%d=%ls\n", i, argv[i]);  /* app.exe / hello world / foo\bar */
    LocalFree(argv);

    /* _getdrive() is environment-dependent (Wine maps the Unix cwd to Z:=26, real
       Windows/ARET's C:-rooted model = 3) -> shimmed sound but not oracle-compared. */
    char nb[32];
    _ui64toa_s(0xFFFFFFFFFFFFFFFFULL, nb, sizeof nb, 16);
    printf("u64=%s\n", nb);                            /* ffffffffffffffff */
    char mb[4] = {0};
    int r = wctomb(mb, L'A');
    printf("wctomb=%d %c\n", r, mb[0]);                /* 1 A */
    printf("kbhit=%d\n", _kbhit());                    /* 0 */
    printf("done\n");
    return 0;
}
