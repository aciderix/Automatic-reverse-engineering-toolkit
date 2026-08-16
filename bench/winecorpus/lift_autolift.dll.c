/* Companion DLL for the --auto-lift gate: a plain non-system DLL the exe imports.
 * --auto-lift must DISCOVER it (by reading the exe's import table), find the file
 * beside the exe, and lift it -- with no explicit --with-dll (doc 81 I2.b). */
__declspec(dllexport) int dll_answer(int x) { return x * 2 + 1; }
