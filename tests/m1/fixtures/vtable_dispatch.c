/* C model of a C++ vtable: a static const array of method pointers in .rodata,
   an object holding a pointer to it, and dispatch through obj->vt->method(obj) —
   exactly what a C++ virtual call compiles to (`call [vtable + k]`). */
#include <stdio.h>


struct Shape; 
struct VT { int (*area)(const struct Shape*); const char* (*name)(void); };
struct Shape { const struct VT* vt; int a, b; };

static int  sq_area(const struct Shape* s){ return s->a * s->a; }
static const char* sq_name(void){ return "square"; }
static int  rc_area(const struct Shape* s){ return s->a * s->b; }
static const char* rc_name(void){ return "rect"; }

static const struct VT SQ = { sq_area, sq_name };
static const struct VT RC = { rc_area, rc_name };

int total(struct Shape** v, int n){
    int t = 0;
    for (int i = 0; i < n; i++) {
        printf("%s=%d ", v[i]->vt->name(), v[i]->vt->area(v[i]));  /* virtual dispatch */
        t += v[i]->vt->area(v[i]);
    }
    return t;
}
int main(void){
    struct Shape sq = { &SQ, 5, 0 };
    struct Shape rc = { &RC, 3, 4 };
    struct Shape* shapes[2] = { &sq, &rc };
    printf("\ntotal = %d\n", total(shapes, 2));
    return 0;
}
