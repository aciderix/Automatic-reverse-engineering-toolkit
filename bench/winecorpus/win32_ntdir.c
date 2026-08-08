/* ntdll Nt* file floor tranche 3 (doc 82): NtQueryDirectoryFile, FileNamesInformation. That class
 * carries NO environmental fields (no times/sizes) -- only NextEntryOffset, FileIndex, the name --
 * so a directory enumeration is bit-identical Wine. Entries are "." ".." then case-insensitive
 * sorted; single-entry mode returns one per call (Information = 12+namelen), multi-entry packs them
 * 8-aligned (NextEntryOffset = align8(12+namelen), 0 on the last); exhausted -> STATUS_NO_MORE_FILES.
 * Files are created with kernel32 (auto-linked); ntdll does the directory syscalls. */
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
typedef LONG NTSTATUS;
NTSTATUS WINAPI NtQueryDirectoryFile(HANDLE,HANDLE,PVOID,PVOID,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS,BOOLEAN,PUNICODE_STRING,BOOLEAN);
NTSTATUS WINAPI NtClose(HANDLE);
#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE 0x1
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20
#endif

static HANDLE opendir_nt(void){
    WCHAR dp[]=L"\\??\\C:\\aret_ntdir"; UNICODE_STRING du; RtlInitUnicodeString(&du,dp);
    OBJECT_ATTRIBUTES oa; InitializeObjectAttributes(&oa,&du,OBJ_CASE_INSENSITIVE,NULL,NULL);
    IO_STATUS_BLOCK io; HANDLE h=NULL;
    NtCreateFile(&h,GENERIC_READ|SYNCHRONIZE,&oa,&io,NULL,0,FILE_SHARE_READ,1/*FILE_OPEN*/,
                 FILE_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT,NULL,0);
    return h;
}

int main(void){
    CreateDirectoryA("C:\\aret_ntdir",NULL);
    HANDLE f; DWORD wn;
    f=CreateFileA("C:\\aret_ntdir\\zeta.bin",GENERIC_WRITE,0,0,CREATE_ALWAYS,0,0); WriteFile(f,"hello",5,&wn,0); CloseHandle(f);
    f=CreateFileA("C:\\aret_ntdir\\beta.bin",GENERIC_WRITE,0,0,CREATE_ALWAYS,0,0); CloseHandle(f);
    f=CreateFileA("C:\\aret_ntdir\\mu.bin",GENERIC_WRITE,0,0,CREATE_ALWAYS,0,0); CloseHandle(f);
    CreateDirectoryA("C:\\aret_ntdir\\sub",NULL);

    unsigned char buf[512]; IO_STATUS_BLOCK io;

    /* single-entry, NULL pattern: one entry per call, sorted (. .. beta mu sub zeta) */
    HANDLE h=opendir_nt(); BOOLEAN restart=TRUE; int i=0;
    for(;;){
        memset(buf,0,sizeof buf);
        NTSTATUS s=NtQueryDirectoryFile(h,NULL,NULL,NULL,&io,buf,sizeof buf,12,TRUE,NULL,restart);
        restart=FALSE;
        if(s){ printf("single[%d] s=0x%08lX\n",i,(unsigned long)s); break; }
        ULONG next=*(ULONG*)(buf+0), fidx=*(ULONG*)(buf+4), nlen=*(ULONG*)(buf+8);
        printf("single[%d] Info=%lu next=%lu FileIndex=%lu nlen=%lu name=",i,(unsigned long)io.Information,
            (unsigned long)next,(unsigned long)fidx,(unsigned long)nlen);
        for(ULONG j=0;j<nlen/2;j++) putchar((char)*(WCHAR*)(buf+12+2*j));
        putchar('\n'); i++; if(i>20) break;
    }
    NtClose(h);

    /* multi-entry, "*" pattern: all entries packed into one buffer */
    h=opendir_nt();
    WCHAR starw[]=L"*"; UNICODE_STRING star; RtlInitUnicodeString(&star,starw);
    memset(buf,0,sizeof buf);
    NTSTATUS s=NtQueryDirectoryFile(h,NULL,NULL,NULL,&io,buf,sizeof buf,12,FALSE,&star,TRUE);
    printf("multi s=0x%08lX Info=%lu\n",(unsigned long)s,(unsigned long)io.Information);
    ULONG off=0; i=0;
    for(;;){
        unsigned char*p=buf+off; ULONG next=*(ULONG*)(p+0), nlen=*(ULONG*)(p+8);
        printf("  [%d] off=%lu next=%lu nlen=%lu name=",i,(unsigned long)off,(unsigned long)next,(unsigned long)nlen);
        for(ULONG j=0;j<nlen/2;j++) putchar((char)*(WCHAR*)(p+12+2*j));
        putchar('\n');
        if(!next) break; off+=next; i++; if(i>20) break;
    }
    /* the next call exhausts -> STATUS_NO_MORE_FILES */
    s=NtQueryDirectoryFile(h,NULL,NULL,NULL,&io,buf,sizeof buf,12,FALSE,NULL,FALSE);
    printf("multi-after s=0x%08lX\n",(unsigned long)s);
    NtClose(h);

    /* FileBothDirectoryInformation, single-entry: attr/EOF/AllocationSize/name are deterministic;
     * times and the generated 8.3 short name are environmental -> NOT printed. Names are 8.3-fitting
     * so ShortNameLength is 0 both sides. */
    h=opendir_nt(); restart=TRUE; i=0;
    for(;;){
        memset(buf,0,sizeof buf);
        NTSTATUS bs=NtQueryDirectoryFile(h,NULL,NULL,NULL,&io,buf,sizeof buf,3,TRUE,NULL,restart);
        restart=FALSE;
        if(bs){ printf("both[%d] s=0x%08lX\n",i,(unsigned long)bs); break; }
        ULONG attr=*(ULONG*)(buf+56), nlen=*(ULONG*)(buf+60), easz=*(ULONG*)(buf+64);
        unsigned char shortlen=buf[68];
        long long eof=*(long long*)(buf+40), alloc=*(long long*)(buf+48);
        printf("both[%d] Info=%lu attr=0x%lX EOF=%lld Alloc=%lld Ea=%lu shortlen=%u nlen=%lu name=",i,
            (unsigned long)io.Information,(unsigned long)attr,eof,alloc,(unsigned long)easz,shortlen,(unsigned long)nlen);
        for(ULONG j=0;j<nlen/2;j++) putchar((char)*(WCHAR*)(buf+94+2*j));
        putchar('\n'); i++; if(i>20) break;
    }
    NtClose(h);

    DeleteFileA("C:\\aret_ntdir\\zeta.bin");
    DeleteFileA("C:\\aret_ntdir\\beta.bin");
    DeleteFileA("C:\\aret_ntdir\\mu.bin");
    RemoveDirectoryA("C:\\aret_ntdir\\sub");
    RemoveDirectoryA("C:\\aret_ntdir");
    return 0;
}
