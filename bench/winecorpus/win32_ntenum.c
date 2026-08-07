/* ntdll Nt* registry floor tranche 2 (doc 82): enumeration / info on ARET's g_reg tree, the
 * same syscalls beneath advapi32's RegQueryInfoKey/RegEnumKey/RegEnumValue. Round-trips (create
 * -> enumerate) so it matches Wine, whose fresh winediff prefix is empty too. Subkeys and values
 * are created in DELIBERATELY non-alphabetical order (Zebra/Alpha/Mango, zeta/beta/mu) to PROVE
 * the case-insensitive sorted enumeration order Wine uses. LastWriteTime is environmental and is
 * zeroed before printing; short-buffer cases print only status+ResultLength (buffer content is
 * contract-undefined on overflow). Bit-identical Wine (ntdll only; no advapi32 in the link). */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
typedef LONG NTSTATUS;
NTSTATUS WINAPI NtCreateKey(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,ULONG,PUNICODE_STRING,ULONG,PULONG);
NTSTATUS WINAPI NtOpenKey(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES);
NTSTATUS WINAPI NtSetValueKey(HANDLE,PUNICODE_STRING,ULONG,ULONG,PVOID,ULONG);
NTSTATUS WINAPI NtQueryKey(HANDLE,int,PVOID,ULONG,PULONG);
NTSTATUS WINAPI NtEnumerateKey(HANDLE,ULONG,int,PVOID,ULONG,PULONG);
NTSTATUS WINAPI NtEnumerateValueKey(HANDLE,ULONG,int,PVOID,ULONG,PULONG);
NTSTATUS WINAPI NtFlushKey(HANDLE);
NTSTATUS WINAPI NtDeleteKey(HANDLE);
NTSTATUS WINAPI NtClose(HANDLE);

static void pwname(unsigned char *b, ULONG off, ULONG namelen){ /* namelen in bytes */
    for (ULONG j=0;j<namelen/2;j++) putchar((char)*(WCHAR*)(b+off+2*j));
}
static void phex(unsigned char *b, ULONG off, ULONG n){
    for (ULONG j=0;j<n;j++) printf("%02X", b[off+j]);
}

static HANDLE mkkey(HANDLE root, const wchar_t *name){
    UNICODE_STRING us; RtlInitUnicodeString(&us,(PWSTR)name);
    OBJECT_ATTRIBUTES oa; InitializeObjectAttributes(&oa,&us,OBJ_CASE_INSENSITIVE,root,NULL);
    HANDLE h=NULL; ULONG disp=0; NtCreateKey(&h,KEY_ALL_ACCESS,&oa,0,NULL,0,&disp); return h;
}

int main(void){
    UNICODE_STRING rp; RtlInitUnicodeString(&rp,L"\\Registry\\Machine\\Software\\AretNtEnum");
    OBJECT_ATTRIBUTES roa; InitializeObjectAttributes(&roa,&rp,OBJ_CASE_INSENSITIVE,NULL,NULL);
    HANDLE root=NULL; ULONG disp=0;
    NtCreateKey(&root,KEY_ALL_ACCESS,&roa,0,NULL,0,&disp);

    HANDLE z=mkkey(root,L"Zebra"), a=mkkey(root,L"Alpha"), m=mkkey(root,L"Mango");
    NtClose(z); NtClose(a); NtClose(m);
    DWORD d7=7, d99=99; UCHAR bin[5]={1,2,3,4,5};
    UNICODE_STRING vz,vb,vm; RtlInitUnicodeString(&vz,L"zeta"); RtlInitUnicodeString(&vb,L"beta"); RtlInitUnicodeString(&vm,L"mu");
    NtSetValueKey(root,&vz,0,REG_DWORD,&d7,4);
    NtSetValueKey(root,&vb,0,REG_BINARY,bin,5);
    NtSetValueKey(root,&vm,0,REG_DWORD,&d99,4);

    unsigned char buf[256]; ULONG rl;

    /* NtQueryKey Full: counts + maxima (bytes) */
    memset(buf,0,sizeof buf); rl=0;
    NTSTATUS s=NtQueryKey(root,2,buf,sizeof buf,&rl);
    printf("qk-full hr=0x%08lX rl=%lu ClassOff=0x%lX SubKeys=%lu MaxName=%lu Values=%lu MaxVName=%lu MaxVData=%lu\n",
        (unsigned long)s,(unsigned long)rl,(unsigned long)*(ULONG*)(buf+12),(unsigned long)*(ULONG*)(buf+20),
        (unsigned long)*(ULONG*)(buf+24),(unsigned long)*(ULONG*)(buf+32),(unsigned long)*(ULONG*)(buf+36),
        (unsigned long)*(ULONG*)(buf+40));

    /* NtQueryKey Basic: name of the key itself */
    memset(buf,0,sizeof buf); rl=0;
    s=NtQueryKey(root,0,buf,sizeof buf,&rl);
    printf("qk-basic hr=0x%08lX rl=%lu NameLen=%lu name=",(unsigned long)s,(unsigned long)rl,(unsigned long)*(ULONG*)(buf+12));
    pwname(buf,16,*(ULONG*)(buf+12)); putchar('\n');

    /* Subkeys, basic (sorted: Alpha, Mango, Zebra) */
    for (ULONG i=0;;i++){ memset(buf,0,sizeof buf); rl=0;
        s=NtEnumerateKey(root,i,0,buf,sizeof buf,&rl);
        if (s){ printf("enumkey-basic[%lu] hr=0x%08lX\n",(unsigned long)i,(unsigned long)s); break; }
        printf("enumkey-basic[%lu] rl=%lu NameLen=%lu name=",(unsigned long)i,(unsigned long)rl,(unsigned long)*(ULONG*)(buf+12));
        pwname(buf,16,*(ULONG*)(buf+12)); putchar('\n'); }

    /* Subkeys, node (ClassOffset present) */
    for (ULONG i=0;;i++){ memset(buf,0,sizeof buf); rl=0;
        s=NtEnumerateKey(root,i,1,buf,sizeof buf,&rl);
        if (s){ printf("enumkey-node[%lu] hr=0x%08lX\n",(unsigned long)i,(unsigned long)s); break; }
        printf("enumkey-node[%lu] rl=%lu ClassOff=0x%lX NameLen=%lu name=",(unsigned long)i,(unsigned long)rl,
            (unsigned long)*(ULONG*)(buf+12),(unsigned long)*(ULONG*)(buf+20));
        pwname(buf,24,*(ULONG*)(buf+20)); putchar('\n'); }

    /* Values, full (sorted: beta, mu, zeta) */
    for (ULONG i=0;;i++){ memset(buf,0,sizeof buf); rl=0;
        s=NtEnumerateValueKey(root,i,1,buf,sizeof buf,&rl);
        if (s){ printf("enumval-full[%lu] hr=0x%08lX\n",(unsigned long)i,(unsigned long)s); break; }
        ULONG doff=*(ULONG*)(buf+8), dlen=*(ULONG*)(buf+12), nlen=*(ULONG*)(buf+16);
        printf("enumval-full[%lu] rl=%lu type=%lu DataOff=%lu DataLen=%lu NameLen=%lu name=",(unsigned long)i,
            (unsigned long)rl,(unsigned long)*(ULONG*)(buf+4),(unsigned long)doff,(unsigned long)dlen,(unsigned long)nlen);
        pwname(buf,20,nlen); printf(" data="); phex(buf,doff,dlen); putchar('\n'); }

    /* Values, basic */
    for (ULONG i=0;;i++){ memset(buf,0,sizeof buf); rl=0;
        s=NtEnumerateValueKey(root,i,0,buf,sizeof buf,&rl);
        if (s){ printf("enumval-basic[%lu] hr=0x%08lX\n",(unsigned long)i,(unsigned long)s); break; }
        printf("enumval-basic[%lu] rl=%lu type=%lu NameLen=%lu name=",(unsigned long)i,(unsigned long)rl,
            (unsigned long)*(ULONG*)(buf+4),(unsigned long)*(ULONG*)(buf+8));
        pwname(buf,12,*(ULONG*)(buf+8)); putchar('\n'); }

    /* Values, partial */
    for (ULONG i=0;;i++){ memset(buf,0,sizeof buf); rl=0;
        s=NtEnumerateValueKey(root,i,2,buf,sizeof buf,&rl);
        if (s){ printf("enumval-partial[%lu] hr=0x%08lX\n",(unsigned long)i,(unsigned long)s); break; }
        ULONG dlen=*(ULONG*)(buf+8);
        printf("enumval-partial[%lu] rl=%lu type=%lu DataLen=%lu data=",(unsigned long)i,(unsigned long)rl,
            (unsigned long)*(ULONG*)(buf+4),(unsigned long)dlen); phex(buf,12,dlen); putchar('\n'); }

    /* Short-buffer contracts (status + ResultLength only) */
    rl=0; s=NtQueryKey(root,2,buf,8,&rl);                    printf("qk-full small(8) hr=0x%08lX rl=%lu\n",(unsigned long)s,(unsigned long)rl);
    rl=0; s=NtEnumerateKey(root,0,0,buf,8,&rl);              printf("enumkey-basic small(8) hr=0x%08lX rl=%lu\n",(unsigned long)s,(unsigned long)rl);
    rl=0; s=NtEnumerateKey(root,0,0,buf,20,&rl);             printf("enumkey-basic mid(20) hr=0x%08lX rl=%lu\n",(unsigned long)s,(unsigned long)rl);
    rl=0; s=NtEnumerateValueKey(root,0,1,buf,8,&rl);         printf("enumval-full small(8) hr=0x%08lX rl=%lu\n",(unsigned long)s,(unsigned long)rl);

    /* NtDeleteKey a subkey by handle, then re-count */
    HANDLE zk=NULL; UNICODE_STRING zn; RtlInitUnicodeString(&zn,L"Zebra");
    OBJECT_ATTRIBUTES zoa; InitializeObjectAttributes(&zoa,&zn,OBJ_CASE_INSENSITIVE,root,NULL);
    s=NtOpenKey(&zk,KEY_ALL_ACCESS,&zoa); printf("open Zebra hr=0x%08lX\n",(unsigned long)s);
    s=NtDeleteKey(zk); printf("delkey Zebra hr=0x%08lX\n",(unsigned long)s); NtClose(zk);
    memset(buf,0,sizeof buf); rl=0; NtQueryKey(root,2,buf,sizeof buf,&rl);
    printf("after-del SubKeys=%lu\n",(unsigned long)*(ULONG*)(buf+20));

    s=NtFlushKey(root); printf("flush hr=0x%08lX\n",(unsigned long)s);

    /* cleanup: delete remaining subkeys + root (leave the prefix clean) */
    HANDLE h;
    h=mkkey(root,L"Alpha"); NtDeleteKey(h); NtClose(h);
    h=mkkey(root,L"Mango"); NtDeleteKey(h); NtClose(h);
    NtDeleteKey(root); NtClose(root);
    return 0;
}
