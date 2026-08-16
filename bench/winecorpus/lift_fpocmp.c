/* Proves ARET recovers a lone FPO function reached only via a .data function pointer
 * (the companion DLL's qsort comparator). If `cmp` is not recovered, the indirect
 * qsort callback aborts and no sorted output appears; a correct sort proves recovery
 * found and lifted the FPO leaf through the data pointer. */
#include <stdio.h>

__declspec(dllimport) void dll_sort(int *arr, int n);

int main(void) {
    int a[] = { 5, 2, 8, 1, 9, 3, 7, 4, 6, 0 };
    int n = sizeof(a) / sizeof(a[0]);
    dll_sort(a, n);
    for (int i = 0; i < n; i++) printf("%d%s", a[i], i + 1 < n ? " " : "\n");
    printf("done\n");
    return 0;
}
