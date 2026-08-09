/* GNU/Itanium C++ EH — SUBTYPE catch (brick 2b): `throw Derived` caught by `catch(Base&)`.
 * The thrown type_info and the caught type_info differ, so the pointer-equality match of
 * brick 2a is not enough; aret_cxa_throw walks the thrown type_info's base chain (classifying
 * each by its ABI vtable pointer — __si/__vmi/__class, emitted from the imports) and matches
 * when the caught type is a base. User-defined types keep the type_info objects LOCAL to the
 * exe (no libstdc++ needed at runtime), isolating the subtype logic. The catch reads the base
 * subobject (offset 0 here), proving the bind. Deterministic => bit-identical Wine. */
#include <cstdio>
struct Base { virtual ~Base() {} int b = 11; };
struct Derived : Base { int d = 22; };
int main() {
    printf("start\n");
    int caught = 0;
    try {
        throw Derived();
    } catch (Base& x) {
        caught = x.b;              // read the base subobject
        printf("caught base b=%d\n", x.b);
    }
    printf("done caught=%d\n", caught);
    return 0;
}
