/* Dynamic module loading (the plugin ecosystem: gdk-pixbuf loaders, GTK/GIO/pango
 * modules, COM inproc servers): LoadLibrary a lifted DLL, resolve an export by name
 * with GetProcAddress, call through it. Exercises ARET's per-module handle + lifted-
 * export resolution. The companion is built as dynloaddll.dll and lifted via
 * --with-dll; main links no import from it (pure runtime load). */
#include <windows.h>
#include <stdio.h>
int main(void){
    HMODULE h = LoadLibraryA("dynloaddll.dll");
    if(!h){ printf("load fail %lu\n",(unsigned long)GetLastError()); return 1; }
    typedef int (*fn1)(int); typedef int (*fn2)(int,int);
    fn1 f = (fn1)GetProcAddress(h, "plugin_func");
    fn2 g = (fn2)GetProcAddress(h, "plugin_add");
    void *miss = (void*)GetProcAddress(h, "nope");   /* absent -> NULL */
    if(!f || !g){ printf("gpa fail %lu\n",(unsigned long)GetLastError()); return 2; }
    printf("f=%d g=%d miss=%d\n", f(10), g(20,22), miss==NULL);
    FreeLibrary(h);
    return 0;
}
