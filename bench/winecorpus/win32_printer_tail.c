/* Printer spooler long-tail (winspool.drv, display-free, measured vs Wine). Not about
 * printing: these are the enumeration/open entry points a program calls to discover
 * printers, and headless there are NONE (no spooler / CUPS) — a deterministic state.
 *   - EnumPrinters (levels 1/2) -> TRUE, 0 needed, 0 returned (empty).
 *   - GetDefaultPrinter -> FALSE + ERROR_FILE_NOT_FOUND (no default).
 *   - OpenPrinter(NULL) -> the local print server (a valid handle), ClosePrinter TRUE.
 *   - OpenPrinter(bogus name) -> FALSE + ERROR_INVALID_PRINTER_NAME.
 *   - ClosePrinter(bogus) -> FALSE + ERROR_INVALID_HANDLE.
 * A program sees "no printers" and takes its graceful path instead of aborting. */
#include <windows.h>
#include <winspool.h>
#include <stdio.h>

int main(void) {
    DWORD needed2 = 0, count2 = 0;
    BOOL e2 = EnumPrintersA(PRINTER_ENUM_LOCAL, NULL, 2, NULL, 0, &needed2, &count2);
    DWORD needed1 = 0, count1 = 0;
    BOOL e1 = EnumPrintersA(PRINTER_ENUM_LOCAL, NULL, 1, NULL, 0, &needed1, &count1);
    printf("enum2 ret=%d needed=%lu count=%lu | enum1 ret=%d needed=%lu count=%lu\n",
           e2, (unsigned long)needed2, (unsigned long)count2,
           e1, (unsigned long)needed1, (unsigned long)count1);

    char buf[256]; DWORD cch = 256;
    BOOL gd = GetDefaultPrinterA(buf, &cch);
    printf("getdefault ret=%d err=%lu cch=%lu\n",
           gd, gd ? 0UL : (unsigned long)GetLastError(), (unsigned long)cch);

    HANDLE hs = NULL;
    BOOL os = OpenPrinterA(NULL, &hs, NULL);           /* local print server */
    BOOL cs = os ? ClosePrinter(hs) : 0;
    printf("open_server ret=%d handle=%d close=%d\n", os, hs != NULL, cs);

    HANDLE hb = NULL;
    BOOL ob = OpenPrinterA("NoSuchPrinter_XYZ", &hb, NULL);
    printf("open_bogus ret=%d err=%lu\n", ob, ob ? 0UL : (unsigned long)GetLastError());

    BOOL cb = ClosePrinter((HANDLE)0x1234);
    printf("close_bogus ret=%d err=%lu\n", cb, cb ? 0UL : (unsigned long)GetLastError());
    printf("done\n");
    return 0;
}
