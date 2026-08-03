#include <windows.h>
#include <winternl.h>
#include <stdio.h>
/* prototypes for the functions provided by the compiled Wine rtlstr.o */
VOID  WINAPI RtlInitAnsiString(PANSI_STRING,PCSZ);
VOID  WINAPI RtlInitUnicodeString(PUNICODE_STRING,PCWSTR);
NTSTATUS WINAPI RtlAnsiStringToUnicodeString(PUNICODE_STRING,PCANSI_STRING,BOOLEAN);
NTSTATUS WINAPI RtlUnicodeStringToAnsiString(PANSI_STRING,PCUNICODE_STRING,BOOLEAN);
NTSTATUS WINAPI RtlIntegerToChar(ULONG,ULONG,ULONG,PCHAR);
BOOLEAN  WINAPI RtlEqualUnicodeString(PCUNICODE_STRING,PCUNICODE_STRING,BOOLEAN);
static void pw(const char*tag,UNICODE_STRING*u){ printf("%s len=%u \"",tag,u->Length);
    for(int i=0;i<u->Length/2;i++)putchar((char)u->Buffer[i]); printf("\"\n"); }
int main(void){
    ANSI_STRING a; RtlInitAnsiString(&a,"Hello");
    printf("initA len=%u max=%u str=%s\n",a.Length,a.MaximumLength,a.Buffer);
    UNICODE_STRING u; RtlAnsiStringToUnicodeString(&u,&a,TRUE); pw("a2u",&u);
    ANSI_STRING b; RtlUnicodeStringToAnsiString(&b,&u,TRUE);
    printf("u2a len=%u str=%s\n",b.Length,b.Buffer);
    char buf[16]; RtlIntegerToChar(0xABCD,16,sizeof buf,buf); printf("int2char=%s\n",buf);
    UNICODE_STRING x,y; RtlInitUnicodeString(&x,L"Foo"); RtlInitUnicodeString(&y,L"foo");
    printf("equal(ci)=%d equal(cs)=%d\n",RtlEqualUnicodeString(&x,&y,TRUE),RtlEqualUnicodeString(&x,&y,FALSE));
    return 0;
}
