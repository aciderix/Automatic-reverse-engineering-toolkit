/* Rect math (Intersect/Union/PtIn/Offset/IsEmpty), char case (CharUpper/Lower),
 * IsBadCodePtr. All exact & deterministic vs Wine. */
#include <windows.h>
#include <stdio.h>
int main(void) {
    RECT a = {10, 10, 100, 100}, b = {50, 50, 200, 200}, r, u;
    int inter = IntersectRect(&r, &a, &b);            /* sequenced before the read */
    printf("inter=%d r=%ld,%ld,%ld,%ld\n", inter, r.left, r.top, r.right, r.bottom);
    UnionRect(&u, &a, &b);
    printf("union=%ld,%ld,%ld,%ld\n", u.left, u.top, u.right, u.bottom);
    RECT e = {5, 5, 5, 10}; printf("empty=%d\n", IsRectEmpty(&e));
    POINT p = {60, 60}, q = {5, 5};
    printf("ptin=%d ptout=%d\n", PtInRect(&a, p), PtInRect(&a, q));
    RECT of = {0, 0, 10, 10}; OffsetRect(&of, 3, 4);
    printf("offset=%ld,%ld,%ld,%ld\n", of.left, of.top, of.right, of.bottom);
    char up[16] = "Hello", lo[16] = "Hello"; CharUpperA(up); CharLowerA(lo);
    printf("upper=[%s] lower=[%s]\n", up, lo);
    printf("badcode=%d\n", IsBadCodePtr((FARPROC)main) != 0);
    printf("done\n");
    return 0;
}
