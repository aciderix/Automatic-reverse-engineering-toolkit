/* GObject reproduction/characterization fixture. Splits the two paths so ARET's
 * behaviour pins exactly WHERE the libffi wall bites:
 *   A) type system + property + an EXPLICIT c-marshaller signal (g_cclosure_marshal
 *      VOID__INT) — should NOT need libffi;
 *   B) a GENERIC-marshaller signal (c_marshaller = NULL -> g_cclosure_marshal_generic
 *      -> ffi_call) then emit — this is the path that requires libffi.
 * Deterministic printf output. Build stage B under a macro so we can test A alone. */
#include <glib-object.h>
#include <stdio.h>

/* --- a minimal GObject subclass --- */
#define MY_TYPE_THING (my_thing_get_type())
G_DECLARE_FINAL_TYPE(MyThing, my_thing, MY, THING, GObject)
struct _MyThing { GObject parent; int val; };
G_DEFINE_TYPE(MyThing, my_thing, G_TYPE_OBJECT)
static void my_thing_class_init(MyThingClass *k) { (void)k; }
static void my_thing_init(MyThing *o) { o->val = 42; }

static int g_hits = 0;
static void on_ping(MyThing *o, int n, gpointer u) { (void)o; (void)u; g_hits += n; }

int main(void) {
    /* A) type system */
    MyThing *t = g_object_new(MY_TYPE_THING, NULL);
    printf("typename=%s is_a=%d val=%d\n",
           G_OBJECT_TYPE_NAME(t),
           G_TYPE_CHECK_INSTANCE_TYPE(t, MY_TYPE_THING) ? 1 : 0,
           t->val);

    /* A) explicit-marshaller signal */
    guint sig = g_signal_new("ping", MY_TYPE_THING, G_SIGNAL_RUN_LAST, 0,
                             NULL, NULL, g_cclosure_marshal_VOID__INT,
                             G_TYPE_NONE, 1, G_TYPE_INT);
    g_signal_connect(t, "ping", G_CALLBACK(on_ping), NULL);
    g_signal_emit(t, sig, 0, 7);
    g_signal_emit(t, sig, 0, 3);
    printf("explicit_marshal hits=%d\n", g_hits);

#ifdef STAGE_B
    /* B) generic-marshaller signal -> libffi */
    g_hits = 0;
    guint sig2 = g_signal_new("pong", MY_TYPE_THING, G_SIGNAL_RUN_LAST, 0,
                              NULL, NULL, NULL /* generic -> ffi */,
                              G_TYPE_NONE, 1, G_TYPE_INT);
    g_signal_connect(t, "pong", G_CALLBACK(on_ping), NULL);
    g_signal_emit(t, sig2, 0, 5);
    printf("generic_marshal hits=%d\n", g_hits);
#endif

    g_object_unref(t);
    printf("done\n");
    return 0;
}
