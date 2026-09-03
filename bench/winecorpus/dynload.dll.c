/* Companion plugin for dynload.c: a dynamically-loaded (LoadLibrary) module whose
 * export is reached by name via GetProcAddress. Lifted under ARET with --with-dll. */
__declspec(dllexport) int plugin_func(int x){ return x*3 + 7; }
__declspec(dllexport) int plugin_add(int a, int b){ return a + b; }
