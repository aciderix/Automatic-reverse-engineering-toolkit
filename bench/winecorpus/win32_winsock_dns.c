/* Winsock2 name-resolution path (doc 90, increment 2): getaddrinfo/freeaddrinfo
 * (Windows ADDRINFOA layout rebuilt in guest memory), inet_ntoa (pointer return),
 * gethostname. Deterministic inputs only — a numeric address (AI_NUMERICHOST, no
 * DNS) and a literal — so ARET and Wine match bit-for-bit. gethostname is compared
 * as a boolean (the host name is the same container for both engines but its exact
 * form is environmental). */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    int rc = getaddrinfo("127.0.0.1", "80", &hints, &res);
    printf("gai=%d res=%d\n", rc, res != NULL);
    if (res) {
        struct sockaddr_in *si = (struct sockaddr_in *)res->ai_addr;
        printf("fam=%d type=%d addrlen=%d addr=%08lx port=%d next=%d\n",
               res->ai_family, res->ai_socktype, (int)res->ai_addrlen,
               (unsigned long)ntohl(si->sin_addr.s_addr), ntohs(si->sin_port),
               res->ai_next != NULL);
        freeaddrinfo(res);
    }

    /* AI_PASSIVE with NULL node -> wildcard 0.0.0.0. */
    struct addrinfo ph, *pr = NULL;
    memset(&ph, 0, sizeof ph);
    ph.ai_family = AF_INET;
    ph.ai_socktype = SOCK_STREAM;
    ph.ai_flags = AI_PASSIVE;
    int rc2 = getaddrinfo(NULL, "0", &ph, &pr);
    if (pr) {
        struct sockaddr_in *si = (struct sockaddr_in *)pr->ai_addr;
        printf("passive gai=%d addr=%08lx\n", rc2, (unsigned long)ntohl(si->sin_addr.s_addr));
        freeaddrinfo(pr);
    }

    struct in_addr ia;
    ia.s_addr = htonl(0x7F000001);
    printf("ntoa=%s\n", inet_ntoa(ia));

    char hn[128] = {0};
    int hr = gethostname(hn, sizeof hn);
    printf("gethostname=%d ok=%d\n", hr, hn[0] != 0);

    WSACleanup();
    printf("done\n");
    return 0;
}
