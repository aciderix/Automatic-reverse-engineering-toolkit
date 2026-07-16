/* sscanf — used by real binaries (sqlite/busybox import it). Integers (d/i/u/x/o +
 * l/ll/h), floats, %s (width), %c, scanset %[...], suppression %*, %n, return value,
 * EOF. Oracle: Wine. */
#include <stdio.h>
int main(void){
    int a,b,c,r; long long ll; unsigned u; short h; char s1[32],s2[32],ch; double d; float f;
    r=sscanf("12 34 56", "%d %d %d", &a,&b,&c); printf("r=%d %d %d %d\n", r,a,b,c);
    r=sscanf("0x1F 077 42", "%x %o %d", &a,&b,&c); printf("r=%d %d %d %d\n", r,a,b,c);
    r=sscanf("100200300400", "%lld", &ll); printf("r=%d %lld\n", r,ll);
    r=sscanf("-5 65535", "%hd %u", &h,&u); printf("r=%d %d %u\n", r,h,u);
    r=sscanf("3.14 2.5e3", "%lf %f", &d,&f); printf("r=%d %.2f %.1f\n", r,d,f);
    r=sscanf("hello world", "%s %s", s1,s2); printf("r=%d [%s][%s]\n", r,s1,s2);
    r=sscanf("abcdef", "%3s", s1); printf("r=%d [%s]\n", r,s1);
    r=sscanf("X", "%c", &ch); printf("r=%d [%c]\n", r,ch);
    r=sscanf("key=val", "%[^=]=%s", s1,s2); printf("r=%d [%s][%s]\n", r,s1,s2);
    r=sscanf("99 88", "%*d %d", &a); printf("r=%d %d\n", r,a);
    int n; r=sscanf("abc123", "abc%d%n", &a,&n); printf("r=%d %d n=%d\n", r,a,n);
    r=sscanf("nope", "%d", &a); printf("r=%d\n", r);
    r=sscanf("", "%d", &a); printf("r_eof=%d\n", r);
    r=sscanf("  7", "%d", &a); printf("r=%d %d\n", r,a);
    printf("done\n");
    return 0;
}
