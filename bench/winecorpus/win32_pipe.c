/* CreatePipe guard: an anonymous pipe maps to POSIX pipe(); the two HANDLEs are
 * read/write fds that WriteFile/ReadFile/CloseHandle drive directly. A small
 * write (< the pipe buffer) then a read round-trips within one process without
 * blocking. All printed values are booleans/counts — identical under Wine. */
#include <windows.h>
#include <stdio.h>

int main(void) {
    HANDLE r = NULL, w = NULL;
    BOOL ok = CreatePipe(&r, &w, NULL, 0);
    printf("pipe created=%d\n", ok ? 1 : 0);

    DWORD wr = 0, rd = 0;
    char buf[16] = {0};
    BOOL wok = WriteFile(w, "hello", 5, &wr, NULL);
    BOOL rok = ReadFile(r, buf, 5, &rd, NULL);
    printf("write ok=%d n=%lu  read ok=%d n=%lu content=%s\n",
           wok ? 1 : 0, (unsigned long)wr, rok ? 1 : 0, (unsigned long)rd, buf);

    CloseHandle(r);
    CloseHandle(w);
    return 0;
}
