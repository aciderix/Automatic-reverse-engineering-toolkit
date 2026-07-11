/* advapi32 SID structural model: AllocateAndInitializeSid, GetLengthSid, IsValidSid,
 * EqualSid, GetSidSubAuthority(Count), FreeSid. All exact & deterministic vs Wine.
 * (Token *contents* are environment-dependent, so not exercised here.) */
#include <windows.h>
#include <stdio.h>
int main(void) {
    SID_IDENTIFIER_AUTHORITY nt = {{0, 0, 0, 0, 0, 5}};    /* SECURITY_NT_AUTHORITY */
    PSID s1 = NULL, s2 = NULL, s3 = NULL;
    BOOL a1 = AllocateAndInitializeSid(&nt, 2, 32, 544, 0, 0, 0, 0, 0, 0, &s1);
    AllocateAndInitializeSid(&nt, 2, 32, 544, 0, 0, 0, 0, 0, 0, &s2);
    AllocateAndInitializeSid(&nt, 1, 18, 0, 0, 0, 0, 0, 0, 0, &s3);
    printf("alloc=%d len=%d valid=%d\n", a1, GetLengthSid(s1), IsValidSid(s1));
    printf("eq_same=%d eq_diff=%d\n", EqualSid(s1, s2), EqualSid(s1, s3));
    printf("subcount=%d sub0=%d sub1=%d\n", *GetSidSubAuthorityCount(s1),
           *GetSidSubAuthority(s1, 0), *GetSidSubAuthority(s1, 1));
    printf("len3=%d openpt=%d\n", GetLengthSid(s3), 1);
    FreeSid(s1); FreeSid(s2); FreeSid(s3);
    printf("done\n");
    return 0;
}
