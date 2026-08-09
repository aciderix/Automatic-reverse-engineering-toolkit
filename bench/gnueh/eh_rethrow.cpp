// GNU/Itanium C++ EH fixture: __cxa_rethrow (`throw;` inside a catch, re-caught by an
// enclosing try in the same function). Exercises the frame lifecycle where the establisher
// frame stays live across the inner catch, and the shared landing pad runs __cxa_end_catch
// (of the inner catch(...)) BEFORE __cxa_begin_catch of the outer catch(int) -- the in-flight
// exception must survive that end_catch (Itanium: __cxa_rethrow negates handlerCount).
#include <cstdio>
int main() {
    printf("start\n");
    try {
        try {
            throw 7;
        } catch (...) {
            printf("inner\n");
            throw;
        }
    } catch (int v) {
        printf("outer %d\n", v);
    }
    printf("done\n");
    return 0;
}
