/* GENERATED companion to nt_types.h (doc 82 tranche 6): the registry/token type surface a
 * whole compiled Wine dlls/ntdll/reg.c needs, plus NTAPI declarations of the floor/rtlstr
 * functions it calls (so the compiled Wine code uses stdcall, matching the floor -- an
 * implicit cdecl decl would imbalance the stack). Measured by compiling reg.c against it. */
#ifndef __ARET_REG_TYPES_H
#define __ARET_REG_TYPES_H
typedef ULONG ACCESS_MASK;
typedef HANDLE *PHANDLE;
typedef union _LARGE_INTEGER { struct { DWORD LowPart; LONG HighPart; } u; LONGLONG QuadPart; } LARGE_INTEGER, *PLARGE_INTEGER;
typedef struct _OBJECT_ATTRIBUTES { ULONG Length; HANDLE RootDirectory; UNICODE_STRING *ObjectName;
    ULONG Attributes; PVOID SecurityDescriptor, SecurityQualityOfService; } OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;
#define OBJ_INHERIT 0x00000002
#define OBJ_CASE_INSENSITIVE 0x00000040
#define OBJ_OPENIF 0x00000080
#define OBJ_OPENLINK 0x00000100
#define InitializeObjectAttributes(p,n,a,r,s) do{ (p)->Length=sizeof(OBJECT_ATTRIBUTES); \
    (p)->RootDirectory=(r); (p)->Attributes=(a); (p)->ObjectName=(n); (p)->SecurityDescriptor=(s); \
    (p)->SecurityQualityOfService=NULL; }while(0)
typedef enum _KEY_INFORMATION_CLASS { KeyBasicInformation, KeyNodeInformation, KeyFullInformation,
    KeyNameInformation, KeyCachedInformation, KeyFlagsInformation } KEY_INFORMATION_CLASS;
typedef enum _KEY_VALUE_INFORMATION_CLASS { KeyValueBasicInformation, KeyValueFullInformation,
    KeyValuePartialInformation, KeyValueFullInformationAlign64, KeyValuePartialInformationAlign64 } KEY_VALUE_INFORMATION_CLASS;
typedef struct _KEY_VALUE_PARTIAL_INFORMATION { ULONG TitleIndex, Type, DataLength; UCHAR Data[1]; } KEY_VALUE_PARTIAL_INFORMATION, *PKEY_VALUE_PARTIAL_INFORMATION;
typedef struct _KEY_VALUE_FULL_INFORMATION { ULONG TitleIndex, Type, DataOffset, DataLength, NameLength; WCHAR Name[1]; } KEY_VALUE_FULL_INFORMATION, *PKEY_VALUE_FULL_INFORMATION;
typedef struct _KEY_BASIC_INFORMATION { LARGE_INTEGER LastWriteTime; ULONG TitleIndex, NameLength; WCHAR Name[1]; } KEY_BASIC_INFORMATION, *PKEY_BASIC_INFORMATION;
typedef NTSTATUS (WINAPI *PRTL_QUERY_REGISTRY_ROUTINE)(PCWSTR,ULONG,PVOID,ULONG,PVOID,PVOID);
typedef struct _RTL_QUERY_REGISTRY_TABLE { PRTL_QUERY_REGISTRY_ROUTINE QueryRoutine; ULONG Flags;
    PWSTR Name; PVOID EntryContext; ULONG DefaultType; PVOID DefaultData; ULONG DefaultLength; } RTL_QUERY_REGISTRY_TABLE, *PRTL_QUERY_REGISTRY_TABLE;
typedef enum { TokenUser=1, TokenGroups, TokenPrivileges, TokenOwner } TOKEN_INFORMATION_CLASS;
typedef PVOID PSID;
typedef struct _SID_AND_ATTRIBUTES { PSID Sid; DWORD Attributes; } SID_AND_ATTRIBUTES;
typedef struct _TOKEN_USER { SID_AND_ATTRIBUTES User; } TOKEN_USER, *PTOKEN_USER;
#define REG_NONE 0
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define REG_BINARY 3
#define REG_DWORD 4
#define REG_DWORD_LITTLE_ENDIAN 4
#define REG_MULTI_SZ 7
#define RTL_REGISTRY_ABSOLUTE 0
#define RTL_REGISTRY_SERVICES 1
#define RTL_REGISTRY_CONTROL 2
#define RTL_REGISTRY_WINDOWS_NT 3
#define RTL_REGISTRY_DEVICEMAP 4
#define RTL_REGISTRY_USER 5
#define RTL_REGISTRY_OPTIONAL 0x20000000
#define RTL_REGISTRY_HANDLE 0x40000000
#define RTL_QUERY_REGISTRY_SUBKEY 0x00000001
#define RTL_QUERY_REGISTRY_TOPKEY 0x00000002
#define RTL_QUERY_REGISTRY_REQUIRED 0x00000004
#define RTL_QUERY_REGISTRY_NOVALUE 0x00000008
#define RTL_QUERY_REGISTRY_NOEXPAND 0x00000010
#define RTL_QUERY_REGISTRY_DIRECT 0x00000020
#define RTL_QUERY_REGISTRY_DELETE 0x00000040
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034)
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035)
#define STATUS_NO_MORE_ENTRIES ((NTSTATUS)0x8000001A)
#define STATUS_INVALID_HANDLE ((NTSTATUS)0xC0000008)
#define STATUS_OBJECT_PATH_SYNTAX_BAD ((NTSTATUS)0xC000003B)
#define KEY_READ 0x20019
#define KEY_WRITE 0x20006
#define KEY_ALL_ACCESS 0xF003F
#define GENERIC_READ 0x80000000
#define GENERIC_WRITE 0x40000000
#define MAXIMUM_ALLOWED 0x02000000
#define TOKEN_QUERY 0x0008

#define OBJ_PERMANENT 0x00000010
#define OBJ_EXCLUSIVE 0x00000020
#define REG_LINK 6
#define REG_OPTION_NON_VOLATILE 0x00000000
#define REG_OPTION_VOLATILE 0x00000001
#define REG_CREATED_NEW_KEY 1
#define REG_OPENED_EXISTING_KEY 2
#define SID_MAX_SUB_AUTHORITIES 15
typedef BYTE *LPBYTE;
typedef struct _SID { BYTE Revision; BYTE SubAuthorityCount; BYTE IdentifierAuthority[6];
    DWORD SubAuthority[SID_MAX_SUB_AUTHORITIES]; } SID, *PSID_;
#define PtrToUlong(p) ((ULONG)(ULONG_PTR)(p))
#define FIELD_OFFSET(t,f) ((long)(long*)&(((t*)0)->f))
typedef UNICODE_STRING *PUNICODE_STRING;

/* External floor/rtlstr functions reg.c calls -- MUST be declared NTAPI (stdcall) so the compiled
 * Wine code uses the right calling convention (an implicit cdecl decl imbalances the stack -> crash). */
void* NTAPI RtlAllocateHeap(void*,ULONG,size_t);
void* NTAPI RtlFreeHeap(void*,ULONG,void*);
void* NTAPI GetProcessHeap(void);
void  NTAPI RtlInitUnicodeString(PUNICODE_STRING,PCWSTR);
NTSTATUS NTAPI RtlAppendUnicodeToString(PUNICODE_STRING,PCWSTR);
BOOLEAN  NTAPI RtlCreateUnicodeString(PUNICODE_STRING,PCWSTR);
void  NTAPI RtlFreeUnicodeString(PUNICODE_STRING);

/* The Nt* registry syscalls reg.c calls -- declared NTAPI so reg.c uses stdcall, matching the
 * ntdll_ntreg.c floor wrappers (an implicit cdecl decl would imbalance the stack / clobber eax). */
NTSTATUS NTAPI NtCreateKey(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,ULONG,PUNICODE_STRING,ULONG,PULONG);
NTSTATUS NTAPI NtOpenKey(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES);
NTSTATUS NTAPI NtSetValueKey(HANDLE,PUNICODE_STRING,ULONG,ULONG,const void*,ULONG);
NTSTATUS NTAPI NtQueryValueKey(HANDLE,PUNICODE_STRING,KEY_VALUE_INFORMATION_CLASS,void*,ULONG,ULONG*);
NTSTATUS NTAPI NtDeleteValueKey(HANDLE,PUNICODE_STRING);
NTSTATUS NTAPI NtDeleteKey(HANDLE);
NTSTATUS NTAPI NtEnumerateKey(HANDLE,ULONG,KEY_INFORMATION_CLASS,void*,ULONG,ULONG*);
NTSTATUS NTAPI NtEnumerateValueKey(HANDLE,ULONG,KEY_VALUE_INFORMATION_CLASS,void*,ULONG,ULONG*);
NTSTATUS NTAPI NtClose(HANDLE);
NTSTATUS NTAPI NtQueryInformationToken(HANDLE,int,void*,ULONG,ULONG*);
#endif
