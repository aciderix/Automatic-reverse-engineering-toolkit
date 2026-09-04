/* RTTI __dynamic_cast stress: down-cast, cross-cast (sideways across multiple bases), and
 * a cast to/through a VIRTUAL base — all on a polymorphic diamond. Pure lifted libstdc++
 * __dynamic_cast walking lifted vtables/type_info. */
#include <cstdio>
struct V { virtual ~V() {} virtual const char* who() { return "V"; } int vx = 5; };
struct A : virtual V { int a = 1; const char* who() override { return "A"; } };
struct B : virtual V { int b = 2; const char* who() override { return "B"; } };
struct C : A, B { int c = 3; const char* who() override { return "C"; } };

int main() {
    setvbuf(stdout, 0, _IONBF, 0);
    printf("start\n");
    C c;
    V* pv = &c;                          /* implicit up-cast to the virtual base */
    printf("who=%s vx=%d\n", pv->who(), pv->vx);
    C* pc = dynamic_cast<C*>(pv);         /* down-cast through virtual base -> C */
    printf("down C=%d c=%d\n", pc != nullptr, pc ? pc->c : -1);
    A* pa = dynamic_cast<A*>(pv);         /* cross to A subobject */
    B* pb = dynamic_cast<B*>(pv);         /* cross to B subobject */
    printf("A=%d a=%d  B=%d b=%d\n", pa != nullptr, pa ? pa->a : -1, pb != nullptr, pb ? pb->b : -1);
    A* pa2 = &c;
    B* pb2 = dynamic_cast<B*>(pa2);       /* true cross-cast A* -> B* (sideways) */
    printf("cross A->B=%d b=%d\n", pb2 != nullptr, pb2 ? pb2->b : -1);
    V* pv2 = dynamic_cast<V*>(pa2);       /* up-cast to virtual base via dynamic_cast */
    printf("A->V=%d vx=%d\n", pv2 != nullptr, pv2 ? pv2->vx : -1);
    printf("done\n");
    return 0;
}
