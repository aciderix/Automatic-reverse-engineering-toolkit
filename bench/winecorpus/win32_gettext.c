/* gettext (libintl) C-locale / no-catalog identity, measured post-lift wall (doc 90).
 * With no .mo translation catalog loaded (the C locale / default), gettext returns the
 * msgid unchanged. ARET routes libintl_* to HLE shims; the Wine oracle loads the real
 * libintl-8.dll (via NAME.winedll) with no catalog -> same identity result. Declared
 * locally (no libintl.h in the base mingw). */
#include <stdio.h>
extern char *libintl_gettext(const char *);
extern char *libintl_dgettext(const char *, const char *);
extern char *libintl_ngettext(const char *, const char *, unsigned long);
extern char *libintl_textdomain(const char *);
extern char *libintl_bindtextdomain(const char *, const char *);

int main(void) {
    printf("gettext=%s\n", libintl_gettext("Hello, world"));
    printf("dgettext=%s\n", libintl_dgettext("dom", "Goodbye"));
    printf("ngettext1=%s\n", libintl_ngettext("%d file", "%d files", 1));
    printf("ngettextN=%s\n", libintl_ngettext("%d file", "%d files", 5));
    printf("textdomain=%s\n", libintl_textdomain("myapp"));
    printf("bindtextdomain=%s\n", libintl_bindtextdomain("myapp", "/usr/share/locale"));
    printf("done\n");
    return 0;
}
