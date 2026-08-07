/* Heavy-form end-to-end (doc 82): a PE importing ntdll Rtl* string functions. Under ARET
 * these route to the REAL Wine bodies compiled from runtime/wine_heavy/rtlstr.c (via the
 * aret_Rtl* adapters), standing on the ASCII floor; under Wine they hit the real ntdll.
 * Bit-identical => a whole Wine ntdll source file, compiled unchanged into the autonomous
 * ELF, runs correctly. ASCII only (the proven subset). */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>

/* Declarations mingw's winternl.h omits (else the call is implicit cdecl and mismatches the
 * NTAPI/@N symbol in libntdll — the same header/lib skew gen_win32_sigs surfaced). */
NTSTATUS WINAPI RtlIntegerToChar(ULONG, ULONG, ULONG, PCHAR);
NTSTATUS WINAPI RtlCharToInteger(PCSZ, ULONG, PULONG);
BOOLEAN  WINAPI RtlCreateUnicodeStringFromAsciiz(PUNICODE_STRING, PCSZ);
BOOLEAN  WINAPI RtlEqualUnicodeString(PCUNICODE_STRING, PCUNICODE_STRING, BOOLEAN);

static void pu(const char *tag, UNICODE_STRING *u) {
    printf("%s len=%u \"", tag, u->Length);
    for (unsigned i = 0; i < u->Length / 2; i++) putchar((char)u->Buffer[i]);
    printf("\"\n");
}

int main(void) {
    ANSI_STRING a;
    RtlInitAnsiString(&a, "Hello, World");
    printf("initA len=%u max=%u str=%s\n", a.Length, a.MaximumLength, a.Buffer);

    UNICODE_STRING u;
    RtlAnsiStringToUnicodeString(&u, &a, TRUE);
    pu("a2u", &u);

    ANSI_STRING b;
    RtlUnicodeStringToAnsiString(&b, &u, TRUE);
    printf("u2a len=%u str=%s\n", b.Length, b.Buffer);
    RtlFreeUnicodeString(&u);
    RtlFreeAnsiString(&b);

    char buf[24];
    RtlIntegerToChar(0xABCD1234u, 16, sizeof buf, buf);
    printf("int2char(hex)=%s\n", buf);
    RtlIntegerToChar(1000000u, 10, sizeof buf, buf);
    printf("int2char(dec)=%s\n", buf);

    ULONG v = 0;
    RtlCharToInteger("42", 10, &v);
    printf("char2int=%lu\n", (unsigned long)v);

    UNICODE_STRING x, y, z;
    RtlInitUnicodeString(&x, L"Foo");
    RtlInitUnicodeString(&y, L"foo");
    RtlInitUnicodeString(&z, L"Foo");
    printf("eq(Foo,foo,ci)=%d eq(Foo,foo,cs)=%d eq(Foo,Foo,cs)=%d\n",
           RtlEqualUnicodeString(&x, &y, TRUE),
           RtlEqualUnicodeString(&x, &y, FALSE),
           RtlEqualUnicodeString(&x, &z, FALSE));

    UNICODE_STRING c;
    if (RtlCreateUnicodeStringFromAsciiz(&c, "made")) {
        pu("fromAsciiz", &c);
        RtlFreeUnicodeString(&c);
    }
    return 0;
}
