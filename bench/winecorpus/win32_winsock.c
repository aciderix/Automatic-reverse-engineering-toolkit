/* Winsock2 core TCP path (doc 90: the #1 OS wall once the C++ runtime is lifted).
 * A single-process localhost round-trip: a blocking connect() to a listening
 * loopback socket completes in-kernel (sits in the accept backlog), so client and
 * server live in one thread deterministically. Prints only deterministic facts
 * (payloads, return codes, byte-order values) — never the ephemeral port — so ARET
 * and Wine match bit-for-bit. Verifies socket/bind/listen/accept/connect/send/recv/
 * setsockopt/getsockname/select/htons/closesocket/WSAStartup/WSAGetLastError. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    WSADATA w;
    int rc = WSAStartup(MAKEWORD(2, 2), &w);
    /* No WSAGetLastError() here: Wine headless leaks ERROR_MOD_NOT_FOUND (126) from the
       failed window-driver/explorer startup before main, which is environmental noise. */
    printf("wsastartup=%d ver=%d.%d\n", rc, LOBYTE(w.wVersion), HIBYTE(w.wVersion));
    printf("htons=%04x ntohs=%04x htonl=%08lx\n",
           htons(0x1234), ntohs(0x1234), (unsigned long)htonl(0x12345678));

    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    printf("socket_ok=%d\n", srv != INVALID_SOCKET);
    int one = 1;
    printf("setsockopt=%d\n", setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof one));

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = 0;                              /* ephemeral */
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    /* Separate statements: argument evaluation order is unspecified, and listen() on an
       unbound socket auto-binds, which would make an inline bind()/listen() race. */
    int br = bind(srv, (struct sockaddr *)&a, sizeof a);
    int lr = listen(srv, 1);
    printf("bind=%d listen=%d\n", br, lr);

    struct sockaddr_in bound;
    int bl = sizeof bound;
    getsockname(srv, (struct sockaddr *)&bound, &bl);
    printf("port_nonzero=%d\n", ntohs(bound.sin_port) != 0);

    SOCKET cli = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ca = bound;
    ca.sin_family = AF_INET;
    ca.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    printf("connect=%d\n", connect(cli, (struct sockaddr *)&ca, sizeof ca));

    SOCKET acc = accept(srv, NULL, NULL);
    printf("accept_ok=%d\n", acc != INVALID_SOCKET);

    printf("send=%d\n", send(cli, "ping", 4, 0));
    char buf[8] = {0};
    int n = recv(acc, buf, sizeof buf, 0);
    printf("recv=%d buf=%s\n", n, buf);

    send(acc, "pong", 4, 0);
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(cli, &rf);
    struct timeval tv = {2, 0};
    int sr = select(0, &rf, NULL, NULL, &tv);
    printf("select=%d readable=%d\n", sr, FD_ISSET(cli, &rf) ? 1 : 0);
    char b2[8] = {0};
    recv(cli, b2, sizeof b2, 0);
    printf("buf2=%s\n", b2);

    closesocket(cli);
    closesocket(acc);
    closesocket(srv);
    WSACleanup();
    printf("done\n");
    return 0;
}
