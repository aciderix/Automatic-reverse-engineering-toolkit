// GNU/Itanium C++ EH fixture: a handler THROWS A NEW exception (not a rethrow). The new
// throw's __cxa_throw overwrites the in-flight exception state, yet GCC's shared landing pad
// runs the inner handler's closing __cxa_end_catch (destroying the OLD object, E(1)) BEFORE
// the outer handler's __cxa_begin_catch. This guards the snapshot model: end_catch destroys
// the exception captured at begin_catch, not whatever is currently being dispatched.
#include <cstdio>
struct E {
    int v;
    E(int x) : v(x) { printf("E(%d)\n", v); }
    E(const E& o) : v(o.v) { printf("Ecopy(%d)\n", v); }
    ~E() { printf("~E(%d)\n", v); }
};
int main() {
    printf("start\n");
    try {
        try {
            throw E(1);
        } catch (E& e) {
            printf("caught1 %d\n", e.v);
            throw E(2);
        }
    } catch (E& e) {
        printf("caught2 %d\n", e.v);
    }
    printf("done\n");
    return 0;
}
