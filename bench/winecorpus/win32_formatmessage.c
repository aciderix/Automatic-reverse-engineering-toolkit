/* FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM) turns an error code into its text. ARET's
 * strings are extracted verbatim from Wine (our oracle), so the common error codes are
 * bit-identical (message + trailing ".\r\n" + returned length); FORMAT_MESSAGE_ALLOCATE_BUFFER
 * LocalAllocs the result. A code not in the verified table aborts soundly. Expected
 * identical under Wine and ARET. */
#include <windows.h>
#include <stdio.h>
static void one(unsigned code) {
    char buf[256]; memset(buf, 0, sizeof buf);
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, code, 0, buf, sizeof buf, NULL);
    printf("code=%u n=%lu msg=", code, (unsigned long)n);
    for (DWORD k = 0; k < n; k++) printf("%02x", (unsigned char)buf[k]);   /* exact bytes incl CRLF */
    printf("\n");
}
int main(void) {
    unsigned codes[] = {0, 2, 3, 5, 6, 8, 32, 87, 112, 122, 183, 1223};
    for (int i = 0; i < (int)(sizeof codes / sizeof codes[0]); i++) one(codes[i]);
    /* ALLOCATE_BUFFER path */
    char *p = NULL;
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER |
                             FORMAT_MESSAGE_IGNORE_INSERTS, NULL, 2, 0, (LPSTR)&p, 0, NULL);
    printf("alloc n=%lu got=%d text=%s", (unsigned long)n, p != NULL, p ? p : "(null)");
    if (p) LocalFree(p);
    return 0;
}
