/* Exercises the .INI profile API (GetPrivateProfileString/Int, WritePrivateProfile
 * String), POSIX-backed. Wine's read semantics — value whitespace trimmed, a
 * surrounding double-quote pair stripped, case-insensitive section/key, missing ->
 * default — are matched; a write/read round-trip is deterministic and checkable
 * bit-for-bit (the read-back values, not the file layout). */
#include <windows.h>
#include <stdio.h>

int main(void) {
    const char *f = ".\\aret.ini";
    WritePrivateProfileStringA("Sec", NULL, NULL, f);            /* clear section (fresh) */
    WritePrivateProfileStringA("Sec", "Name", "Hello World", f);
    WritePrivateProfileStringA("Sec", "Num", "  42  ", f);        /* surrounding spaces */
    WritePrivateProfileStringA("Sec", "Quoted", "\"spaced value\"", f);
    char buf[64];
    printf("name n=%d [%s]\n", GetPrivateProfileStringA("Sec", "Name", "DEF", buf, sizeof buf, f), buf);
    printf("num_str n=%d [%s]\n", GetPrivateProfileStringA("Sec", "Num", "DEF", buf, sizeof buf, f), buf);
    printf("num_int=%d\n", GetPrivateProfileIntA("Sec", "Num", 7, f));
    printf("quoted n=%d [%s]\n", GetPrivateProfileStringA("Sec", "Quoted", "DEF", buf, sizeof buf, f), buf);
    printf("missing n=%d [%s]\n", GetPrivateProfileStringA("Sec", "Missing", "fallback", buf, sizeof buf, f), buf);
    printf("caseins n=%d [%s]\n", GetPrivateProfileStringA("Sec", "NAME", "DEF", buf, sizeof buf, f), buf);
    /* overwrite then read */
    WritePrivateProfileStringA("Sec", "Name", "Second", f);
    printf("rewrite n=%d [%s]\n", GetPrivateProfileStringA("Sec", "Name", "DEF", buf, sizeof buf, f), buf);
    /* delete key -> default */
    WritePrivateProfileStringA("Sec", "Num", NULL, f);
    printf("deleted n=%d [%s]\n", GetPrivateProfileStringA("Sec", "Num", "gone", buf, sizeof buf, f), buf);
    printf("done\n");
    return 0;
}
