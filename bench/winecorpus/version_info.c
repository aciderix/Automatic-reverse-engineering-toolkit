/* Exercises GetFileVersionInfoSizeA/GetFileVersionInfoA/VerQueryValueA on the
 * program's own embedded VS_VERSIONINFO resource (see version_info.rc). Guards
 * the version-info HLE APIs against regression, independently of strings.exe. */
#include <windows.h>
#include <stdio.h>
int main(void) {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof path);
    DWORD h = 0, sz = GetFileVersionInfoSizeA(path, &h);
    printf("size>0: %d\n", sz > 0);
    static unsigned char buf[8192];
    if (!GetFileVersionInfoA(path, 0, sizeof buf, buf)) { printf("get failed\n"); return 1; }
    void *p; UINT len;
    if (VerQueryValueA(buf, "\\VarFileInfo\\Translation", &p, &len) && len >= 4) {
        WORD *t = (WORD *)p;
        char sub[64];
        sprintf(sub, "\\StringFileInfo\\%04x%04x\\", t[0], t[1]);
        const char *keys[] = { "CompanyName", "FileDescription", "FileVersion",
                               "InternalName", "LegalCopyright", "ProductName" };
        for (int i = 0; i < 6; i++) {
            char q[160]; sprintf(q, "%s%s", sub, keys[i]);
            char *v; UINT vl;
            if (VerQueryValueA(buf, q, (void **)&v, &vl)) printf("%s=%s\n", keys[i], v);
            else printf("%s=(none)\n", keys[i]);
        }
    } else printf("no translation\n");
    return 0;
}
