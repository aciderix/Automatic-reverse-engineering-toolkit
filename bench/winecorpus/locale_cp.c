#include <windows.h>
#include <stdio.h>
int main(void){
  CPINFO ci; int gi=GetCPInfo(GetACP(),&ci);
  printf("acp=%u oem=%u valid1252=%d valid437=%d valid99999=%d cpmax=%d\n",
         GetACP(), GetOEMCP(), IsValidCodePage(1252), IsValidCodePage(437),
         IsValidCodePage(99999), gi?(int)ci.MaxCharSize:-1);
  WCHAR s[]=L"Aa1! ";
  WORD ty[8]={0};
  GetStringTypeW(CT_CTYPE1, s, 5, ty);
  printf("upper=%d lower=%d digit=%d punct=%d space=%d\n",
         !!(ty[0]&C1_UPPER), !!(ty[1]&C1_LOWER), !!(ty[2]&C1_DIGIT),
         !!(ty[3]&C1_PUNCT), !!(ty[4]&C1_SPACE));
  WCHAR up[16]; int n=LCMapStringW(LOCALE_USER_DEFAULT, LCMAP_UPPERCASE, L"hello", 5, up, 16);
  char nb[16]; for(int i=0;i<n;i++)nb[i]=(char)up[i]; nb[n]=0;
  printf("upper_map=[%s] n=%d\n", nb, n);
  return 0;
}
