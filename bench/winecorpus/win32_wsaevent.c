/* Winsock async/event (increment 3, libglib residual): WSASocketW, WSACreateEvent,
 * WSAEventSelect/WSAEnumNetworkEvents (poll-based readiness), WSAWaitForMultipleEvents,
 * WSASetEvent. Single-process localhost, deterministic: a pending connection makes the
 * listener's FD_ACCEPT event signal. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);

    SOCKET srv = WSASocketW(AF_INET, SOCK_STREAM, 0, NULL, 0, 0);
    printf("wsasocket_ok=%d\n", srv != INVALID_SOCKET);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    bind(srv, (struct sockaddr *)&a, sizeof a);
    listen(srv, 1);
    struct sockaddr_in bound; int bl = sizeof bound;
    getsockname(srv, (struct sockaddr *)&bound, &bl);

    WSAEVENT ev = WSACreateEvent();
    WSAEventSelect(srv, ev, FD_ACCEPT);

    SOCKET cli = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ca = bound;
    ca.sin_family = AF_INET; ca.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connect(cli, (struct sockaddr *)&ca, sizeof ca);          /* completes in the backlog */

    DWORD r = WSAWaitForMultipleEvents(1, &ev, FALSE, 5000, FALSE);
    printf("wait=%lu\n", (unsigned long)r);                    /* 0 = WSA_WAIT_EVENT_0 */
    WSANETWORKEVENTS ne;
    WSAEnumNetworkEvents(srv, ev, &ne);
    printf("accept_event=%d\n", (ne.lNetworkEvents & FD_ACCEPT) ? 1 : 0);   /* 1 */

    SOCKET acc = accept(srv, NULL, NULL);
    printf("accepted=%d\n", acc != INVALID_SOCKET);            /* 1 */

    WSAEVENT e2 = WSACreateEvent();
    printf("wait_unset=%lu\n", (unsigned long)WSAWaitForMultipleEvents(1, &e2, FALSE, 0, FALSE)); /* 0x102 */
    WSASetEvent(e2);
    printf("wait_set=%lu\n", (unsigned long)WSAWaitForMultipleEvents(1, &e2, FALSE, 0, FALSE));   /* 0 */

    closesocket(cli); closesocket(acc); closesocket(srv);
    WSACloseEvent(ev); WSACloseEvent(e2);
    WSACleanup();
    printf("done\n");
    return 0;
}
