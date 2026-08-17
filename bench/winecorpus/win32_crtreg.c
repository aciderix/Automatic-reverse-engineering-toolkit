/* HLE family: CRT + registry + crypto leftovers of the 2nd-tier OS wall (doc 90,
 * 2026-08-16): _set_error_mode, _wgetenv, RegGetValueW, CryptAcquireContextW. Prints
 * only DETERMINISTIC facts (round-trips, type checks, success bools) so ARET and Wine
 * match bit-for-bit; env values and random bytes are never printed. */
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

int main(void) {
    /* _set_error_mode: round-trip (set a mode, query it back with _REPORT_ERRMODE). */
    _set_error_mode(_OUT_TO_MSGBOX);
    int q = _set_error_mode(_REPORT_ERRMODE);
    printf("sem=%d\n", q == _OUT_TO_MSGBOX);

    /* _wgetenv: set via _wputenv, read back; missing var -> NULL. */
    _wputenv(L"ARETVAR=hello");
    wchar_t *v = _wgetenv(L"ARETVAR");
    printf("wenv=%d wenv_null=%d\n", v && wcscmp(v, L"hello") == 0, _wgetenv(L"ARET_NO_SUCH_VAR") == NULL);

    /* RegGetValueW: set a DWORD, read it back; type restriction; missing value. */
    HKEY hk;
    RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ARETTest", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL);
    DWORD dv = 0x1234;
    RegSetValueExW(hk, L"num", 0, REG_DWORD, (const BYTE *)&dv, sizeof dv);
    RegCloseKey(hk);

    DWORD got = 0, type = 0, cb = sizeof got;
    LONG r = RegGetValueW(HKEY_CURRENT_USER, L"Software\\ARETTest", L"num", RRF_RT_REG_DWORD, &type, &got, &cb);
    printf("reg_get=%d val=%d type=%d\n", r == ERROR_SUCCESS, got == 0x1234, type == REG_DWORD);

    got = 0; cb = sizeof got;
    LONG r2 = RegGetValueW(HKEY_CURRENT_USER, L"Software\\ARETTest", L"num", RRF_RT_REG_SZ, NULL, &got, &cb);
    printf("reg_wrongtype=%d\n", r2 == ERROR_UNSUPPORTED_TYPE);

    LONG r3 = RegGetValueW(HKEY_CURRENT_USER, L"Software\\ARETTest", L"nope", RRF_RT_ANY, NULL, NULL, NULL);
    printf("reg_miss=%d\n", r3 == ERROR_FILE_NOT_FOUND);

    /* CryptAcquireContextW: acquire + release (the random bytes are not printed). */
    HCRYPTPROV prov = 0;
    BOOL ca = CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
    BOOL cr = ca && CryptReleaseContext(prov, 0);
    printf("crypt=%d\n", ca && prov != 0 && cr);

    printf("done\n");
    return 0;
}
