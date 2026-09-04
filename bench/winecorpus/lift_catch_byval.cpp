/* catch BY VALUE of a class with a non-trivial copy ctor: the landing pad must
 * copy-construct the catch parameter from the thrown object (running the user copy ctor),
 * and the thrown object is destroyed at end_catch. A wrong/missing copy = silent wrong value. */
#include <cstdio>
struct Msg {
    int code; int copies;
    Msg(int c): code(c), copies(0) { printf("ctor %d\n", code); }
    Msg(const Msg& o): code(o.code), copies(o.copies + 1) { printf("copy %d\n", code); }
    ~Msg() { printf("dtor %d (copies=%d)\n", code, copies); }
};
static void boom() { throw Msg(42); }
int main() {
    setvbuf(stdout, 0, _IONBF, 0);
    printf("start\n");
    try {
        boom();
    } catch (Msg m) {              /* BY VALUE: copy-constructed from the thrown object */
        printf("caught code=%d copies=%d\n", m.code, m.copies);
    }
    printf("done\n");
    return 0;
}
