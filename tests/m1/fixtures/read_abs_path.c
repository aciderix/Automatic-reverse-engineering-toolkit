/* Reads a file by ABSOLUTE Unix path that already exists on the host. The path
   sandbox used to prepend the ARET prefix to "/"-rooted paths, turning
   "/tmp/x" into "<prefix>/tmp/x" (nonexistent), so a native tool like `cat
   /tmp/x` could never read a real file. translate_path now passes "/"-absolute
   paths through to the real filesystem. The test writes the file first, then
   runs this. Expected: ABSREAD=HELLO_ABS */
#include <stdio.h>
int main(void) {
    FILE *f = fopen("/tmp/aret_abs_fixture.txt", "r");
    if (!f) { printf("ABSREAD=OPEN_FAILED\n"); return 1; }
    char buf[64];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    printf("ABSREAD=%s", buf);
    return 0;
}
