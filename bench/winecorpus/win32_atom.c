/* Atom tables (kernel32/user32): string<->ATOM interning, refcounted, two
   separate tables (local vs global). Verified bit-identical to Wine.
   NB: the GLOBAL table's absolute base is env-dependent (Wine pre-seeds a few
   global atoms) — the ATOM value is an opaque handle, so we assert only RELATIVE
   properties on global, and absolute values only on the LOCAL table (base
   0xC000, deterministic). */
#include <windows.h>
#include <stdio.h>
int main(void) {
    /* --- local table: absolute values are deterministic (0xC000 up) --- */
    ATOM a = AddAtomA("Hello");
    ATOM b = AddAtomA("hello");            /* case-insensitive -> same, refs=2 */
    char buf[64]; UINT n = GetAtomNameA(a, buf, sizeof buf);
    printf("a=%u same=%d name=%s len=%u\n", a, a == b, buf, n);
    printf("world=%u find=%u\n", AddAtomA("World"), FindAtomA("HELLO"));

    /* refcount: two adds -> first delete keeps it, second removes it. Delete
       returns 0 on success (measured). */
    UINT d1 = DeleteAtom(a);
    ATOM still = FindAtomA("Hello");
    UINT d2 = DeleteAtom(a);
    ATOM gone = FindAtomA("Hello");
    printf("del=%u still=%d gone=%u\n", d1, still != 0, gone);

    /* find-miss and bad-atom error codes (measured). */
    SetLastError(0); ATOM miss = FindAtomA("Nope");
    printf("miss=%u misserr=%lu\n", miss, GetLastError());
    SetLastError(0); char bb[8]; UINT bad = GetAtomNameA(0xFFFF, bb, sizeof bb);
    printf("bad=%u baderr=%lu\n", bad, GetLastError());

    /* integer atom: value passes through, name formats as "#N". */
    ATOM ia = AddAtomA((LPCSTR)(uintptr_t)42);
    char ib[16]; UINT iln = GetAtomNameA(42, ib, sizeof ib);
    printf("int=%u intname=%s intlen=%u\n", ia, ib, iln);

    /* too-small buffer: fills partial, returns 0 (measured). */
    ATOM t = AddAtomA("ABCDEFGHIJ");
    char sb[4]; UINT tn = GetAtomNameA(t, sb, 4);
    printf("trunc=%u truncbuf=%s\n", tn, sb);

    /* --- global table: RELATIVE checks only (absolute base is env-dependent) --- */
    ATOM g = GlobalAddAtomA("GlobalX");
    ATOM gf = GlobalFindAtomA("globalx");   /* case-insensitive -> same */
    char gb[64]; UINT gn = GlobalGetAtomNameA(g, gb, sizeof gb);
    printf("gsame=%d gname=%s glen=%u\n", g == gf, gb, gn);
    /* tables are separate: a local atom is invisible to the global table. */
    printf("global_sees_local=%u local_sees_global=%u\n",
           GlobalFindAtomA("World"), FindAtomA("GlobalX"));
    return 0;
}
