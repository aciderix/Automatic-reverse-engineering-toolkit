/* Self-contained NT types for NATIVE gcc (ARET's HLE compiles with cc, not mingw, and Linux
 * has no winnt.h). WCHAR is 16-bit -> compile the Wine object with -fshort-wchar so L"" match. */
#ifndef __ARET_NT_TYPES_H
#define __ARET_NT_TYPES_H
#include <stdint.h>
#include <stddef.h>
typedef uint16_t WCHAR; typedef char CHAR; typedef unsigned char BOOLEAN,UCHAR,BYTE;
typedef unsigned short USHORT,WORD; typedef short SHORT;
typedef uint32_t ULONG,DWORD,ULONG32,DWORD32; typedef int32_t LONG,INT,BOOL,LONG32;
typedef long NTSTATUS; typedef void VOID; typedef void *PVOID,*HANDLE;
typedef size_t SIZE_T; typedef WCHAR *PWSTR,*LPWSTR,*PWCH; typedef const WCHAR *PCWSTR,*LPCWSTR,*PCWCH;
typedef CHAR *PSTR,*LPSTR,*PCH; typedef const CHAR *PCSTR,*LPCSTR,*PCSZ,*PCCH;
typedef unsigned long ULONG_PTR; typedef long LONG_PTR;
typedef struct _STRING { USHORT Length,MaximumLength; PSTR Buffer; } STRING,ANSI_STRING,OEM_STRING,*PSTRING,*PANSI_STRING,*POEM_STRING;
typedef const STRING *PCANSI_STRING,*PCOEM_STRING;
typedef struct _UNICODE_STRING { USHORT Length,MaximumLength; PWSTR Buffer; } UNICODE_STRING,*PUNICODE_STRING;
typedef const UNICODE_STRING *PCUNICODE_STRING;
typedef struct _GUID { ULONG Data1; USHORT Data2,Data3; UCHAR Data4[8]; } GUID,*LPGUID; typedef const GUID *LPCGUID,*REFGUID;
#define WINAPI __attribute__((stdcall))
#define NTAPI  __attribute__((stdcall))
#define CDECL
#define IN
#define OUT
#define OPTIONAL
#define TRUE 1
#define FALSE 0
#ifndef NULL
#define NULL ((void*)0)
#endif
#define MAXUSHORT 0xffff
#define MAXLONG   0x7fffffff
#define ANYSIZE_ARRAY 1
#define NTSYSAPI
#define __msvcrt_long long
#define __msvcrt_ulong unsigned long
/* NT status codes rtlstr.c uses */
#define STATUS_SUCCESS            ((NTSTATUS)0x00000000)
#define STATUS_BUFFER_OVERFLOW    ((NTSTATUS)0x80000005)
#define STATUS_BUFFER_TOO_SMALL   ((NTSTATUS)0xC0000023)
#define STATUS_INVALID_PARAMETER  ((NTSTATUS)0xC000000D)
#define STATUS_INVALID_PARAMETER_2 ((NTSTATUS)0xC00000F0)
#define STATUS_NAME_TOO_LONG      ((NTSTATUS)0xC0000106)
#define STATUS_NOT_FOUND          ((NTSTATUS)0xC0000225)
#define STATUS_NO_MEMORY          ((NTSTATUS)0xC0000017)
#define STATUS_ACCESS_VIOLATION   ((NTSTATUS)0xC0000005)
/* IS_TEXT_UNICODE_* (winnls) */
#define IS_TEXT_UNICODE_ASCII16 1
#define IS_TEXT_UNICODE_STATISTICS 2
#define IS_TEXT_UNICODE_CONTROLS 4
#define IS_TEXT_UNICODE_SIGNATURE 8
#define IS_TEXT_UNICODE_UNICODE_MASK 0x0f
#define IS_TEXT_UNICODE_REVERSE_ASCII16 0x10
#define IS_TEXT_UNICODE_REVERSE_STATISTICS 0x20
#define IS_TEXT_UNICODE_REVERSE_CONTROLS 0x40
#define IS_TEXT_UNICODE_REVERSE_SIGNATURE 0x80
#define IS_TEXT_UNICODE_REVERSE_MASK 0xf0
#define IS_TEXT_UNICODE_ILLEGAL_CHARS 0x100
#define IS_TEXT_UNICODE_ODD_LENGTH 0x200
#define IS_TEXT_UNICODE_NULL_BYTES 0x1000
#define IS_TEXT_UNICODE_NOT_UNICODE_MASK 0x0f00
#define IS_TEXT_UNICODE_NOT_ASCII_MASK 0xf000
#define IS_TEXT_UNICODE_BUFFER_TOO_SMALL 0
typedef void *LPVOID; typedef const void *LPCVOID; typedef CHAR *PCHAR; typedef BYTE *PBYTE,*PUCHAR; typedef WORD *PWORD; typedef DWORD *PDWORD,*PULONG,*LPDWORD; typedef ULONG *PULONG32;
typedef WCHAR *PWCHAR; typedef USHORT *PUSHORT; typedef SHORT *PSHORT; typedef LONG *PLONG; typedef INT *PINT; typedef BOOL *PBOOL;
typedef uint64_t UINT64,ULONG64,ULONGLONG,DWORD64,DWORDLONG; typedef int64_t INT64,LONG64,LONGLONG;
typedef unsigned int UINT; typedef uint32_t LCID,LANGID; typedef float FLOAT; typedef double DOUBLE;
#include <string.h>
#define RtlZeroMemory(d,l) memset((d),0,(l))
#define RtlFillMemory(d,l,f) memset((d),(f),(l))
#define RtlCopyMemory(d,s,l) memcpy((d),(s),(l))
#define RtlMoveMemory(d,s,l) memmove((d),(s),(l))
#ifndef min
#define min(a,b) (((a)<(b))?(a):(b))
#endif
#ifndef max
#define max(a,b) (((a)>(b))?(a):(b))
#endif
#endif
