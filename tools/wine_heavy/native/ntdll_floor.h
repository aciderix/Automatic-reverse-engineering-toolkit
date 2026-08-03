/* Heavy-form: prototypes for the port-once floor, all NTAPI (stdcall), so the compiled Wine
 * object calls them with ONE convention (mingw declares only a subset, leaving the rest
 * implicit=cdecl -> a silent stdcall/cdecl mismatch = stack drift). Included after the NT headers. */
#ifndef __WINE_HEAVY_FLOOR_H
#define __WINE_HEAVY_FLOOR_H
NTSTATUS NTAPI RtlMultiByteToUnicodeN(WCHAR*,ULONG,ULONG*,const char*,ULONG);
NTSTATUS NTAPI RtlUnicodeToMultiByteN(char*,ULONG,ULONG*,const WCHAR*,ULONG);
NTSTATUS NTAPI RtlUpcaseUnicodeToMultiByteN(char*,ULONG,ULONG*,const WCHAR*,ULONG);
NTSTATUS NTAPI RtlOemToUnicodeN(WCHAR*,ULONG,ULONG*,const char*,ULONG);
NTSTATUS NTAPI RtlUnicodeToOemN(char*,ULONG,ULONG*,const WCHAR*,ULONG);
NTSTATUS NTAPI RtlUpcaseUnicodeToOemN(char*,ULONG,ULONG*,const WCHAR*,ULONG);
NTSTATUS NTAPI RtlMultiByteToUnicodeSize(ULONG*,const char*,ULONG);
ULONG    NTAPI RtlOemStringToUnicodeSize(const STRING*);
ULONG    NTAPI RtlUnicodeStringToOemSize(const UNICODE_STRING*);
LONG     NTAPI RtlCompareUnicodeStrings(const WCHAR*,SIZE_T,const WCHAR*,SIZE_T,BOOLEAN);
PVOID    WINAPI GetProcessHeap(void);
#endif
