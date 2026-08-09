/* GNU/Itanium C++ EH — destructor cleanup during unwind (brick 2c): a throw unwinds
 * through scopes with local objects whose destructors must run, in reverse construction
 * order, before the matching catch. aret_cxa_throw transfers to each frame's cleanup
 * landing pad (selector 0) to run its destructors, which end in _Unwind_Resume; that
 * re-enters the dispatcher to continue outward until the catch frame. Exercises both the
 * non-inlined case (a separate throwing frame `f` with its own local Guard) and the
 * establisher frame `main` (its own local Guard runs before the catch body). The landing
 * pad reads the guard via BOTH the frame base and the frame pointer, so both must be the
 * establisher's. Deterministic => bit-identical Wine. */
#include <cstdio>
struct Guard { int id; Guard(int i):id(i){ printf("Guard%d ctor\n", id); } ~Guard(){ printf("Guard%d dtor\n", id); } };
static void f() {
    Guard g(1);          // its dtor must run during unwind
    throw 42;
}
int main() {
    printf("start\n");
    try {
        Guard g(2);      // its dtor must also run during unwind
        f();
        printf("unreachable\n");
    } catch (int v) {
        printf("caught %d\n", v);
    }
    printf("done\n");
    return 0;
}
