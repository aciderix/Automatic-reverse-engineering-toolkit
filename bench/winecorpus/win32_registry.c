/* In-memory registry round-trip (advapi32) — the measured head of the 2026-07-19
 * Levier-0 re-measure (RegCreateKey in 17/29 Win95 binaries). A program writes its
 * settings and reads them back; that round-trip is deterministic and matches Wine.
 * The fixture deletes its test key first so the create disposition is stable across
 * runs (Wine's registry persists between runs), and deletes it at the end.
 * A value never written is honestly ERROR_FILE_NOT_FOUND (sound), not a guess. */
#include <windows.h>
#include <stdio.h>
#define SUB "Software\\ARETRegTest"

int main(void) {
    RegDeleteKeyA(HKEY_CURRENT_USER, SUB);      /* clean prior run (ignore result) */

    HKEY k; DWORD disp = 0;
    LONG rc = RegCreateKeyExA(HKEY_CURRENT_USER, SUB, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, &disp);
    printf("create rc=%ld disp=%lu\n", (long)rc, (unsigned long)disp);

    DWORD dv = 0xCAFE1234;
    RegSetValueExA(k, "num", 0, REG_DWORD, (const BYTE *)&dv, sizeof dv);
    RegSetValueExA(k, "str", 0, REG_SZ, (const BYTE *)"hello", 6);

    DWORD type = 0, cb = 0;
    LONG q0 = RegQueryValueExA(k, "num", NULL, &type, NULL, &cb);         /* size query */
    printf("qnum_size rc=%ld type=%lu cb=%lu\n", (long)q0, (unsigned long)type, (unsigned long)cb);
    DWORD out = 0; cb = sizeof out; type = 0;
    LONG q1 = RegQueryValueExA(k, "num", NULL, &type, (BYTE *)&out, &cb);
    printf("qnum rc=%ld type=%lu cb=%lu val=%08lx\n", (long)q1, (unsigned long)type, (unsigned long)cb, (unsigned long)out);
    char sb[32]; DWORD scb = sizeof sb; type = 0;
    LONG q2 = RegQueryValueExA(k, "str", NULL, &type, (BYTE *)sb, &scb);
    printf("qstr rc=%ld type=%lu cb=%lu val=[%s]\n", (long)q2, (unsigned long)type, (unsigned long)scb, sb);

    LONG qm = RegQueryValueExA(k, "nope", NULL, NULL, NULL, NULL);        /* missing value */
    char tiny[2]; DWORD tcb = sizeof tiny; type = 0;
    LONG qs = RegQueryValueExA(k, "str", NULL, &type, (BYTE *)tiny, &tcb); /* too small */
    printf("qmiss rc=%ld | qsmall rc=%ld cb=%lu\n", (long)qm, (long)qs, (unsigned long)tcb);

    char vn[32]; DWORD vnl = sizeof vn, vt = 0;                            /* enum value 0 */
    LONG en = RegEnumValueA(k, 0, vn, &vnl, NULL, &vt, NULL, NULL);
    printf("enum0 rc=%ld name=[%s] type=%lu nlen=%lu\n", (long)en, vn, (unsigned long)vt, (unsigned long)vnl);

    DWORD nsub = 99, nval = 99;                                            /* key info */
    RegQueryInfoKeyA(k, NULL, NULL, NULL, &nsub, NULL, NULL, &nval, NULL, NULL, NULL, NULL);
    printf("info subkeys=%lu values=%lu\n", (unsigned long)nsub, (unsigned long)nval);

    RegCloseKey(k);
    HKEY k2; DWORD disp2 = 0;                                              /* reopen -> disp 2 */
    RegCreateKeyExA(HKEY_CURRENT_USER, SUB, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k2, &disp2);
    printf("reopen disp=%lu\n", (unsigned long)disp2);

    HKEY kx;                                                               /* open a missing key */
    LONG ox = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\NoSuchARET", 0, KEY_READ, &kx);
    printf("open_missing rc=%ld\n", (long)ox);

    LONG dvr = RegDeleteValueA(k2, "num");
    RegCloseKey(k2);
    LONG dk = RegDeleteKeyA(HKEY_CURRENT_USER, SUB);
    printf("delval rc=%ld delkey rc=%ld\n", (long)dvr, (long)dk);
    printf("done\n");
    return 0;
}
