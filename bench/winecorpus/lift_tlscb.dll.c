/* Companion DLL with a PE TLS callback (registered in .CRT$XLB, the array the loader
 * runs at process/thread attach). A lifted DLL's TLS callback must be invoked by ARET's
 * loader at process attach, exactly as Windows/Wine do — this is the wall a real lifted
 * libglib program hit ("TLS callback not invoked"). The callback sets a flag on
 * DLL_PROCESS_ATTACH; the exported getter lets the app observe that it ran. Referencing
 * mingw's `_tls_used` forces the PE TLS directory to be emitted (pure native TLS — no
 * emutls, so the callback array reaches the loader through the IMAGE_TLS_DIRECTORY). */
#include <windows.h>

static int g_ran = 0;
static int g_reason = -1;

static void NTAPI aret_tls_cb(PVOID h, DWORD reason, PVOID reserved) {
    (void)h; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) { g_ran = 1; g_reason = (int)reason; }
}

/* Force the TLS directory into the image so the loader sees our .CRT$XLB callback. */
extern const IMAGE_TLS_DIRECTORY _tls_used;
__attribute__((used)) static const void *const aret_keep_tls = &_tls_used;

/* Register the callback in the loader's TLS callback array. */
__attribute__((section(".CRT$XLB"), used))
PIMAGE_TLS_CALLBACK aret_tls_cb_slot = aret_tls_cb;

__declspec(dllexport) int tlscb_ran(void)    { return g_ran; }
__declspec(dllexport) int tlscb_reason(void) { return g_reason; }
