/* catch by a VIRTUAL BASE of the thrown type. With virtual inheritance the base
 * subobject's offset in the derived object lives in the vtable (not a fixed compile-time
 * displacement), so the Itanium catch-match must consult it to this-adjust the caught
 * reference. ARET's dispatcher currently skips virtual bases in the subtype walk
 * (aret_unmodelled / no match). Measure ARET vs Wine. */
#include <cstdio>
struct V { virtual ~V() {} int x; V(int v):x(v){} };
struct L : virtual V { int l; L():V(11),l(1){} };
struct R : virtual V { int r; R():V(22),r(2){} };
struct D : L, R { int d; D():V(99),d(3){} };   /* diamond: one shared virtual V */

int main() {
    setvbuf(stdout, 0, _IONBF, 0);
    printf("start\n");
    try {
        throw D();
    } catch (const V& v) {          /* V is a VIRTUAL base of D */
        printf("caught V.x=%d\n", v.x);
    } catch (...) {
        printf("caught other\n");
    }
    printf("done\n");
    return 0;
}
