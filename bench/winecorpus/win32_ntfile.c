/* ntdll Nt* file floor tranche 3 (doc 82): NtCreateFile/NtOpenFile/NtReadFile/NtWriteFile/
 * NtQueryInformationFile on the same POSIX fd model as kernel32 CreateFile/ReadFile. Round-trips
 * (create -> write -> read/query) so it matches Wine, which round-trips its own filesystem. The
 * NT object name is \??\C:\... (DOS-device namespace). IO_STATUS_BLOCK = {Status@0, Information@4}.
 * Struct layouts / disposition->Information / offset advance / status codes are all measured vs
 * Wine; AllocationSize is st_blocks*512, Wine's own stat formula. ntdll only (+ DeleteFileW for
 * cleanup, kernel32 auto-linked). Bit-identical Wine. */
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
typedef LONG NTSTATUS;
NTSTATUS WINAPI NtReadFile(HANDLE,HANDLE,PVOID,PVOID,PIO_STATUS_BLOCK,PVOID,ULONG,PLARGE_INTEGER,PULONG);
NTSTATUS WINAPI NtWriteFile(HANDLE,HANDLE,PVOID,PVOID,PIO_STATUS_BLOCK,PVOID,ULONG,PLARGE_INTEGER,PULONG);
NTSTATUS WINAPI NtOpenFile(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,ULONG,ULONG);
NTSTATUS WINAPI NtClose(HANDLE);
#ifndef FILE_CREATE
#define FILE_OPEN 1
#define FILE_CREATE 2
#define FILE_OVERWRITE_IF 5
#endif
#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20
#define FILE_NON_DIRECTORY_FILE 0x40
#endif

int main(void){
    WCHAR path[]=L"\\??\\C:\\aret_ntfile.bin";
    UNICODE_STRING us; RtlInitUnicodeString(&us,path);
    OBJECT_ATTRIBUTES oa; InitializeObjectAttributes(&oa,&us,OBJ_CASE_INSENSITIVE,NULL,NULL);
    IO_STATUS_BLOCK io; HANDLE h=NULL;
    ULONG opts=FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE;

    NTSTATUS s=NtCreateFile(&h,GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE,&oa,&io,NULL,FILE_ATTRIBUTE_NORMAL,
                            FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_OVERWRITE_IF,opts,NULL,0);
    printf("create s=0x%08lX Info=%lu\n",(unsigned long)s,(unsigned long)io.Information);

    char data[]="Hello, Nt file!"; /* 15 bytes */
    s=NtWriteFile(h,NULL,NULL,NULL,&io,data,sizeof data-1,NULL,NULL);
    printf("write s=0x%08lX Info=%lu\n",(unsigned long)s,(unsigned long)io.Information);

    /* read at explicit offset 7 (position then advances to 15) */
    char buf[64]; memset(buf,0,sizeof buf);
    LARGE_INTEGER off; off.QuadPart=7;
    s=NtReadFile(h,NULL,NULL,NULL,&io,buf,8,&off,NULL);
    printf("read@7 s=0x%08lX Info=%lu buf=[%.*s]\n",(unsigned long)s,(unsigned long)io.Information,(int)io.Information,buf);

    /* FileStandardInformation */
    unsigned char si[32]; memset(si,0,sizeof si);
    s=NtQueryInformationFile(h,&io,si,sizeof si,FileStandardInformation);
    printf("qstd s=0x%08lX Info=%lu Alloc=%llu EOF=%llu Links=%lu DelPend=%u Dir=%u\n",
        (unsigned long)s,(unsigned long)io.Information,(unsigned long long)*(long long*)(si+0),
        (unsigned long long)*(long long*)(si+8),(unsigned long)*(ULONG*)(si+16),si[20],si[21]);

    /* FilePositionInformation: current offset should be 15 */
    unsigned char pi[16]; memset(pi,0,sizeof pi);
    s=NtQueryInformationFile(h,&io,pi,sizeof pi,FilePositionInformation);
    printf("qpos s=0x%08lX Info=%lu CurOff=%llu\n",(unsigned long)s,(unsigned long)io.Information,(unsigned long long)*(long long*)pi);

    /* sequential read from current position after seeking to 0 then reading 5 */
    LARGE_INTEGER z; z.QuadPart=0; NtReadFile(h,NULL,NULL,NULL,&io,buf,5,&z,NULL);
    memset(buf,0,sizeof buf);
    s=NtReadFile(h,NULL,NULL,NULL,&io,buf,6,NULL,NULL);
    printf("read-seq s=0x%08lX Info=%lu buf=[%.*s]\n",(unsigned long)s,(unsigned long)io.Information,(int)io.Information,buf);

    /* read past EOF -> STATUS_END_OF_FILE */
    LARGE_INTEGER farr; farr.QuadPart=1000;
    s=NtReadFile(h,NULL,NULL,NULL,&io,buf,8,&farr,NULL);
    printf("read-eof s=0x%08lX Info=%lu\n",(unsigned long)s,(unsigned long)io.Information);
    NtClose(h);

    /* reopen existing via NtOpenFile, read all */
    h=NULL; s=NtOpenFile(&h,GENERIC_READ|SYNCHRONIZE,&oa,&io,FILE_SHARE_READ,opts);
    printf("open s=0x%08lX Info=%lu\n",(unsigned long)s,(unsigned long)io.Information);
    memset(buf,0,sizeof buf);
    LARGE_INTEGER z2; z2.QuadPart=0;
    s=NtReadFile(h,NULL,NULL,NULL,&io,buf,64,&z2,NULL);
    printf("open-read s=0x%08lX Info=%lu buf=[%.*s]\n",(unsigned long)s,(unsigned long)io.Information,(int)io.Information,buf);
    NtClose(h);

    /* FILE_CREATE on existing -> STATUS_OBJECT_NAME_COLLISION */
    h=NULL; s=NtCreateFile(&h,GENERIC_WRITE|SYNCHRONIZE,&oa,&io,NULL,FILE_ATTRIBUTE_NORMAL,0,FILE_CREATE,opts,NULL,0);
    printf("create-collide s=0x%08lX\n",(unsigned long)s);
    if (h) NtClose(h);

    /* open a nonexistent file -> STATUS_OBJECT_NAME_NOT_FOUND */
    WCHAR nf[]=L"\\??\\C:\\aret_ntfile_missing.bin"; UNICODE_STRING un; RtlInitUnicodeString(&un,nf);
    OBJECT_ATTRIBUTES na; InitializeObjectAttributes(&na,&un,OBJ_CASE_INSENSITIVE,NULL,NULL);
    h=NULL; s=NtOpenFile(&h,GENERIC_READ|SYNCHRONIZE,&na,&io,FILE_SHARE_READ,opts);
    printf("open-missing s=0x%08lX\n",(unsigned long)s);

    /* NtSetInformationFile: truncate (FileEndOfFile) + seek (FilePosition) */
    h=NULL; s=NtCreateFile(&h,GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE,&oa,&io,NULL,FILE_ATTRIBUTE_NORMAL,
                           FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_OVERWRITE_IF,opts,NULL,0);
    char ten[]="0123456789"; NtWriteFile(h,NULL,NULL,NULL,&io,ten,10,NULL,NULL);
    LARGE_INTEGER eof; eof.QuadPart=4;
    s=NtSetInformationFile(h,&io,&eof,8,FileEndOfFileInformation);
    printf("set-eof s=0x%08lX Info=%lu\n",(unsigned long)s,(unsigned long)io.Information);
    unsigned char si2[32]; NtQueryInformationFile(h,&io,si2,sizeof si2,FileStandardInformation);
    printf("after-eof EOF=%llu\n",(unsigned long long)*(long long*)(si2+8));
    LARGE_INTEGER pos; pos.QuadPart=2;
    s=NtSetInformationFile(h,&io,&pos,8,FilePositionInformation);
    printf("set-pos s=0x%08lX Info=%lu\n",(unsigned long)s,(unsigned long)io.Information);
    char rb[16]; memset(rb,0,sizeof rb);
    s=NtReadFile(h,NULL,NULL,NULL,&io,rb,8,NULL,NULL);   /* from pos 2 of a 4-byte file -> "23" */
    printf("read-at-pos s=0x%08lX Info=%lu buf=[%.*s]\n",(unsigned long)s,(unsigned long)io.Information,(int)io.Information,rb);
    LARGE_INTEGER grow; grow.QuadPart=8;
    NtSetInformationFile(h,&io,&grow,8,FileEndOfFileInformation);
    NtQueryInformationFile(h,&io,si2,sizeof si2,FileStandardInformation);
    printf("after-grow EOF=%llu\n",(unsigned long long)*(long long*)(si2+8));
    NtClose(h);

    /* FileDispositionInformation (delete-on-close): create a file, mark for delete, close, reopen -> gone */
    WCHAR dp[]=L"\\??\\C:\\aret_ntdel.bin"; UNICODE_STRING du; RtlInitUnicodeString(&du,dp);
    OBJECT_ATTRIBUTES da; InitializeObjectAttributes(&da,&du,OBJ_CASE_INSENSITIVE,NULL,NULL);
    h=NULL; s=NtCreateFile(&h,GENERIC_WRITE|DELETE|SYNCHRONIZE,&da,&io,NULL,FILE_ATTRIBUTE_NORMAL,0,FILE_OVERWRITE_IF,opts,NULL,0);
    char dd[]="temp"; NtWriteFile(h,NULL,NULL,NULL,&io,dd,4,NULL,NULL);
    struct { unsigned char DeleteFile; } fdi; fdi.DeleteFile=1;
    s=NtSetInformationFile(h,&io,&fdi,1,FileDispositionInformation);
    printf("set-disp s=0x%08lX Info=%lu\n",(unsigned long)s,(unsigned long)io.Information);
    NtClose(h);
    h=NULL; s=NtOpenFile(&h,GENERIC_READ|SYNCHRONIZE,&da,&io,FILE_SHARE_READ,opts);
    printf("reopen-after-delete s=0x%08lX\n",(unsigned long)s);
    if (h) NtClose(h);

    DeleteFileW(L"C:\\aret_ntfile.bin");
    return 0;
}
