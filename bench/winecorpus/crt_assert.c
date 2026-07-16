/* _assert (msvcrt) — a failing assert must TERMINATE, not fall through. Oracle:
 * Wine. Observable on stdout: "before" prints, then the program aborts at the
 * failed assertion, so "after" never prints. (The weak stub returned 0, which let
 * the program run past a violated invariant — a silent wrong result.) The assert
 * message and exit code go to stderr / the process status, which winediff does not
 * compare; the stdout truncation is the proof the abort happened. */
#include <assert.h>
#include <stdio.h>
int main(void) {
    printf("before\n");
    fflush(stdout);
    int x = 2;
    assert(x == 3 && "x must be 3");
    printf("after\n"); /* unreachable: the assert above aborts */
    fflush(stdout);
    return 0;
}
