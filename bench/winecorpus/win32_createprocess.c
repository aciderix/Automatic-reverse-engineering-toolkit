/* CreateProcessA is a SOUND FAILURE in ARET: launching a Windows child .exe has no
 * faithful native model, so it returns FALSE (never a simulation, never an abort) and
 * a caller takes its error path. Wine actually attempts the launch; for a path whose
 * directory does not exist both fail identically (FALSE). The fixture prints only the
 * BOOL (the error code and any real-launch success are path/environment dependent and
 * outside this bit-exact check). Expected identical: r=0. */
#include <windows.h>
#include <stdio.h>
int main(void) {
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
    BOOL r = CreateProcessA("Z:\\aret_no_such_dir\\nope.exe", NULL, NULL, NULL,
                            FALSE, 0, NULL, NULL, &si, &pi);
    printf("createprocess r=%d\n", r);
    return 0;
}
