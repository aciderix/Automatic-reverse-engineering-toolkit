#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void){
  char drive[8],dir[256],fname[128],ext[64];
  _splitpath("C:\\foo\\bar\\baz.txt",drive,dir,fname,ext);
  printf("drive=[%s] dir=[%s] fname=[%s] ext=[%s]\n",drive,dir,fname,ext);
  char out[512]; _makepath(out,"D:","\\a\\b\\","file","dat");
  printf("makepath=[%s]\n",out);
  printf("fullpath_ok=%d\n", _fullpath(out,"x.txt",sizeof out)!=NULL && strlen(out)>5);
  return 0;
}
