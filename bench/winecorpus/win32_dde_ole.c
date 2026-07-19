/* Two high-breadth quick-wins from the 2026-07-19 Levier-0 re-measure (display-free):
 *  - OLE/COM init depth: OleInitialize (which inits COM) returns S_OK, then a nested
 *    CoInitialize returns S_FALSE(1); CoUninitialize/OleUninitialize unwind.
 *  - DDE param packing: for WM_DDE_ACK/DATA (allocating messages) Pack/Unpack round-
 *    trips the two values; for a non-DDE message it is MAKELONG(lo,hi) / LOWORD/HIWORD.
 * The raw allocated handle is a pointer (non-deterministic) so it is NOT printed —
 * only the round-trip results and the MAKELONG form, which are deterministic. */
#include <windows.h>
#include <stdio.h>

int main(void) {
    HRESULT oi = OleInitialize(NULL);
    HRESULT ci = CoInitialize(NULL);          /* nested -> S_FALSE */
    printf("ole=%ld co=%ld\n", (long)oi, (long)ci);
    CoUninitialize();
    OleUninitialize();

    UINT lo = 0x1234, hi = 0xABCD;

    LPARAM p_ack = PackDDElParam(WM_DDE_ACK, lo, hi);      /* allocating message */
    UINT al = 0, ah = 0; BOOL ua = UnpackDDElParam(WM_DDE_ACK, p_ack, &al, &ah);
    printf("ack unpack=%d lo=%04x hi=%04x\n", ua, al, ah);
    printf("ack free=%d\n", FreeDDElParam(WM_DDE_ACK, p_ack));

    LPARAM p_data = PackDDElParam(WM_DDE_DATA, lo, hi);    /* allocating message */
    UINT dl = 0, dh = 0; UnpackDDElParam(WM_DDE_DATA, p_data, &dl, &dh);
    printf("data lo=%04x hi=%04x\n", dl, dh);
    FreeDDElParam(WM_DDE_DATA, p_data);

    LPARAM p_u = PackDDElParam(WM_USER, lo, hi);           /* non-DDE -> MAKELONG */
    UINT xl = 0, xh = 0; UnpackDDElParam(WM_USER, p_u, &xl, &xh);
    printf("user pack=%08lx lo=%04x hi=%04x free=%d\n",
           (unsigned long)p_u, xl, xh, FreeDDElParam(WM_USER, p_u));
    printf("done\n");
    return 0;
}
