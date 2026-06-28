#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
int main(int argc, char**argv){
  printf("argc=%d\n", argc);
  long sum=0;
  for(int i=1;i<argc;i++){ printf("arg%d=[%s]\n", i, argv[i]); sum+=atol(argv[i]); }
  printf("numsum=%ld\n", sum);
  // GetCommandLineW round-trip: count args via CommandLineToArgvW would need shell32;
  // instead just confirm the ASCII command line contains the last arg.
  const char*cl=GetCommandLineA();
  printf("cl_has_last=%d\n", (argc>1 && strstr(cl, argv[argc-1])!=NULL));
  return 0;
}
