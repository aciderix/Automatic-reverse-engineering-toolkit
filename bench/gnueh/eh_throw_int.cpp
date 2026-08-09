/* GNU/Itanium C++ EH differential — the minimal end-to-end throw/catch through ARET's
 * own dispatcher (doc 71, 2026-08-09 [EH] brick 2a). mingw g++ lowers throw/catch to the
 * Itanium ABI (__cxa_allocate_exception / __cxa_throw / __cxa_begin_catch) + a DWARF
 * .eh_frame LSDA; libgcc's real unwinder walks the machine stack via CFI, which the
 * shared-stack model has no equivalent of, so ARET replaces it (analysis::gnu_eh recovers
 * the LSDA, the lifter injects an establish setjmp + an active-call-PC hook, and
 * aret_cxa_throw maps the throwing call site to its landing pad, matches the thrown type,
 * and longjmps there). This exercises the minimal path: `throw int` caught by `catch(int)`
 * (a pointer-equality type match) AND a local set before the throw and read after the catch
 * (proves the continuation runs against the establisher's realigned frame). Deterministic
 * ⇒ bit-identical Wine. Subtype matching (base classes) and destructor cleanup are later
 * bricks; this is the FIRST GNU C++ exception to round-trip through ARET. */
#include <cstdio>

static int f(int x) {
    if (x < 0) throw 42;   // the only throw; caught in main
    return x * 3;
}

int main() {
    printf("start\n");
    int caught = -1, normal = 0;
    try {
        normal = f(7);                       // no throw: 21
        printf("f(7)=%d\n", normal);
        normal = f(-1);                      // throws int 42
        printf("unreachable %d\n", normal);
    } catch (int v) {
        caught = v;
        printf("caught %d\n", v);
    }
    printf("done caught=%d normal=%d\n", caught, normal);
    return 0;
}
