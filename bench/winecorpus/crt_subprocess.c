/* Subprocess via the host shell: _popen/_pclose (read a portable command's output) and
 * system (exit code). Mapped to /bin/sh -c, so a portable command works identically to
 * cmd.exe under Wine; a Windows-only command or a PE child fails to exec -> a real
 * failure, never silent-wrong (the "cannot run a PE child" boundary holds). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    FILE *p = _popen("echo test123", "r");
    char buf[64] = "";
    int got = 0;
    if (p) {
        if (fgets(buf, sizeof buf, p)) got = 1;
        _pclose(p);
    }
    for (char *s = buf; *s; s++)
        if (*s == '\n' || *s == '\r') { *s = 0; break; }
    printf("popen ok:%d out=[%s]\n", got, buf);

    int rc = system("exit 0");
    printf("system rc0:%d\n", rc == 0);
    return 0;
}
