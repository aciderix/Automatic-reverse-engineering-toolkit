/* GetProcAddress of an ntdll export that glib GetProcAddress-probes then calls:
   NtNotifyChangeMultipleKeys (the registry-change-watch syscall beneath
   RegNotifyChangeKeyValue). Wine's ntdll exports it, so GetProcAddress returns a
   real pointer; ARET must too, or a caller that asserts the pointer non-NULL (glib's
   g_win32_registry_key_watch, via g_once_init_leave_pointer) aborts every gio program.
   Regression guard for the ntdll HLE gap: assert the probe resolves. */
#include <windows.h>
#include <stdio.h>
int main(void){
    HMODULE m = GetModuleHandleW(L"ntdll.dll");
    FARPROC p = GetProcAddress(m, "NtNotifyChangeMultipleKeys");
    printf("module=%s proc=%s\n", m ? "ok" : "null", p ? "nonnull" : "null");
    return 0;
}
