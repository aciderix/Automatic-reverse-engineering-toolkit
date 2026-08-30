/* Minimal GLib-core reproduction fixture: exercises the widest post-lift wall
 * (g_strdup, g_string_*, g_hash_table_*, g_strsplit/g_strjoinv, g_free). Output is
 * plain printf (deterministic, byte-exact) so ARET (lifting libglib) and Wine
 * (running the real DLL) can be compared to the byte. No GObject, no regex. */
#include <glib.h>
#include <stdio.h>

int main(void) {
    /* g_strdup + g_free */
    char *s = g_strdup("hello glib");
    printf("dup=%s len=%d\n", s, (int)strlen(s));
    g_free(s);

    /* GString builder */
    GString *b = g_string_new("Hello");
    g_string_append(b, ", ");
    g_string_append_printf(b, "%s!", "World");
    printf("gstring=%s len=%d\n", b->str, (int)b->len);
    g_string_free(b, TRUE);

    /* GHashTable: insert then look up */
    GHashTable *h = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(h, "one", "1");
    g_hash_table_insert(h, "two", "2");
    g_hash_table_insert(h, "three", "3");
    printf("ht size=%d two=%s missing=%s\n",
           g_hash_table_size(h),
           (char *)g_hash_table_lookup(h, "two"),
           g_hash_table_lookup(h, "nope") ? "?" : "(nil)");
    g_hash_table_destroy(h);

    /* g_strsplit / g_strjoinv round trip */
    char **parts = g_strsplit("a,b,c,d", ",", -1);
    int n = 0; while (parts[n]) n++;
    char *joined = g_strjoinv("-", parts);
    printf("split n=%d joined=%s\n", n, joined);
    g_free(joined);
    g_strfreev(parts);

    /* a couple of pure string utils */
    char *up = g_ascii_strup("MixedCase", -1);
    printf("strup=%s\n", up);
    g_free(up);

    printf("done\n");
    return 0;
}
