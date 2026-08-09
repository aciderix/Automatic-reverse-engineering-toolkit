// GNU/Itanium C++ EH fixture: multiple inheritance with a NON-ZERO base offset. C derives
// from A then B, so the B sub-object sits at a non-zero byte offset inside C. `catch (B&)`
// of a `throw C` must apply the Itanium `this`-adjustment (the __vmi_class_type_info base
// offset) so the caught reference points at C's B sub-object, while __cxa_end_catch still
// destroys/frees the ALLOCATION BASE (the whole C), not the interior B pointer.
#include <cstdio>
struct A { int a; A() : a(1) {} virtual ~A() { printf("~A\n"); } };
struct B { int b; B() : b(2) {} virtual ~B() { printf("~B\n"); } };
struct C : A, B { int c; C() : c(3) {} ~C() { printf("~C\n"); } };
int main() {
    printf("start\n");
    try {
        throw C();
    } catch (B& b) {
        printf("caught B b=%d\n", b.b);
    }
    printf("done\n");
    return 0;
}
