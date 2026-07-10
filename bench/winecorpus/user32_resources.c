/* Exercises the PE .rsrc resource APIs (G4, doc 72), display-free: FindResourceA/
 * LoadResource/LockResource/SizeofResource/FreeResource on a custom RT_RCDATA
 * blob, and LoadStringA on a string table (including a string in a second block).
 * ARET walks the real IMAGE_RESOURCE_DIRECTORY tree in the mapped image; Wine
 * reads the same embedded bytes — so the output is bit-identical by construction.
 * Everything printed is deterministic (exact string values, exact blob bytes). */
#include <windows.h>
#include <stdio.h>

int main(void) {
    HMODULE h = GetModuleHandleA(NULL);

    /* Custom RT_RCDATA resource by name. */
    HRSRC r = FindResourceA(h, "MYBLOB", RT_RCDATA);
    printf("find blob=%d\n", r != NULL);
    if (r) {
        DWORD sz = SizeofResource(h, r);
        HGLOBAL g = LoadResource(h, r);
        const unsigned char *p = (const unsigned char *)LockResource(g);
        printf("blob size=%lu bytes=", (unsigned long)sz);
        for (DWORD i = 0; i < sz; i++) printf("%02x", p[i]);
        printf("\n");
        FreeResource(g);
    }

    /* A resource that does not exist -> NULL (sound: no fabricated resource). */
    printf("find missing=%d\n", FindResourceA(h, "NOPE", RT_RCDATA) != NULL);

    /* String table: exact values + returned lengths, across two blocks. */
    int ids[] = { 100, 101, 102, 115, 277 };
    for (int i = 0; i < 5; i++) {
        char buf[64];
        int n = LoadStringA(h, ids[i], buf, sizeof buf);
        printf("str %d n=%d [%s]\n", ids[i], n, buf);
    }

    /* A missing string id -> 0, empty buffer. */
    char buf[16];
    int n = LoadStringA(h, 9999, buf, sizeof buf);
    printf("str missing n=%d [%s]\n", n, buf);

    /* Truncation into a short buffer: returns chars copied (excl NUL). */
    char small[8];
    int m = LoadStringA(h, 100, small, sizeof small);
    printf("str trunc n=%d [%s]\n", m, small);

    printf("done\n");
    return 0;
}
