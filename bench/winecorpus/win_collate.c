/* Linguistic string collation (lstrcmpW / lstrcmpiW / CompareStringW), bit-identical
 * to Wine. Per-character sort weights (incl. the primary-ignorable '-'/'\'' special
 * level) are MEASURED from Wine's LCMAP_SORTKEY and reproduced exactly for a proven
 * ASCII subset; outside it (control chars, non-ASCII) -> sound abort. The identical-
 * string fast path returns 0 for ANY content, so an equality check never aborts. */
#include <windows.h>
#include <stdio.h>
static int sgn(int x){ return x<0?-1:x>0?1:0; }
int main(void){
    const wchar_t *T[] = {
        L"Hello",L"hello",L"HELLO",L"hEllo",L"World",L"abc",L"abd",L"ABC",L"aB",L"Ab",
        L"a",L"A",L"aa",L"aA",L"Aa",L"z",L"Z",L"9",L"0",L"10",L"2",L"apple",L"Apple",
        L"apple1",L"apple2",L"file_1",L"File_2",L"x y",L"x Y",L"",L"a b c",L"abc123",
        L"CamelCase",L"camelcase",L"Test.txt",L"test.TXT",L"~end",L"end~",
        L"read-me",L"readme",L"read-me.txt",L"O'Brien",L"OBrien",L"a-b",L"ab-",L"-ab",
        L"co-op",L"coop",L"e-mail",L"email", 0 };
    unsigned h = 2166136261u; int n = 0;
    for (int i = 0; T[i]; i++) for (int j = 0; T[j]; j++) {
        h = (h ^ (unsigned char)(sgn(lstrcmpW(T[i],T[j])) + 1)) * 16777619u;
        h = (h ^ (unsigned char)(sgn(lstrcmpiW(T[i],T[j])) + 1)) * 16777619u;
        h = (h ^ (unsigned char)(sgn(CompareStringW(LOCALE_USER_DEFAULT,0,T[i],-1,T[j],-1) - 2) + 1)) * 16777619u;
        n++;
    }
    printf("pairs=%d hash=%08x\n", n, h);
    printf("readme/read-me=%d coop/co-op=%d OBrien/O'Brien=%d ab-/-ab=%d\n",
        sgn(lstrcmpW(L"readme",L"read-me")), sgn(lstrcmpW(L"coop",L"co-op")),
        sgn(lstrcmpW(L"OBrien",L"O'Brien")), sgn(lstrcmpW(L"ab-",L"-ab")));
    printf("eq_dash=%d eq_accent=%d\n", lstrcmpW(L"a-b",L"a-b"), lstrcmpW(L"café",L"café"));
    printf("done\n");
    return 0;
}
