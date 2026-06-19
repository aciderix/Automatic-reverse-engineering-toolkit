/* TEB/PEB fixture: read the thread/process environment block through the fs
   segment, as the Windows CRT startup does. The HLE provides a synthetic TEB so
   fs:[0x18] (Self) and fs:[0x30] (PEB) resolve to real, consistent values. */
#include <windows.h>
__declspec(dllimport) int printf(const char *, ...);

void __stdcall mainCRTStartup(void) {
    unsigned teb, peb, self;
    __asm__ volatile ("movl %%fs:0x18, %0" : "=r"(teb));   /* TEB self  */
    __asm__ volatile ("movl %%fs:0x30, %0" : "=r"(peb));   /* PEB ptr   */
    self = *(unsigned *)(teb + 0x18);                       /* TEB->Self */
    printf("TEB: nonnull=%d self_consistent=%d peb_nonnull=%d\n",
           teb != 0, self == teb, peb != 0);
    ExitProcess(0);
}
