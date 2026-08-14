/* LEVIER 1 on the GNU C++ RUNTIME -- step 3c: a destructor (RAII) runs during unwind of
 * an exception thrown from WITHIN lifted libstdc++ -- the everyday pattern. f() holds a
 * local Guard and calls std::vector::at(9) out of range, which throws std::out_of_range
 * from inside libstdc++; the unwind must run ~Guard in f's frame (a cleanup landing pad,
 * no catch -> selector 0), then propagate to main's catch. Exercises the combination of
 * multi-frame unwind + intermediate-frame destructor + a throw originating in libstdc++,
 * all bit-identical to Wine (which loads the same libstdc++ beside the exe). */
#include <cstdio>
#include <vector>
#include <stdexcept>
struct Guard {
    int id;
    Guard(int i) : id(i) { printf("Guard(%d)\n", id); }
    ~Guard() { printf("~Guard(%d)\n", id); }
};
static void f() {
    Guard g(1);                 /* RAII: ~Guard(1) must run during unwind */
    std::vector<int> v;
    v.push_back(7);
    (void)v.at(9);              /* throws std::out_of_range FROM libstdc++ */
    printf("unreachable\n");
}
int main() {
    printf("start\n");
    try {
        f();
    } catch (const std::exception& e) {
        printf("caught: %s\n", e.what());
    }
    printf("done\n");
    return 0;
}
