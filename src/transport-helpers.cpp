#include <string>
#include <cstring>

#include "transport.hpp"
#include "fss-log.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

auto convert_str_to_sa(const std::string &addr, uint16_t port, struct sockaddr_storage *sa) -> bool
{
    int family = AF_UNSPEC;
    /* Try converting an IPv4 address first, then IPv6, then DNS lookup */
    {
        struct in_addr ia = {};
        if (inet_pton(AF_INET, addr.c_str(), &ia) == 1)
        {
            family = AF_INET;
            auto *sa_in = as_sockaddr_in(sa);
            memset(sa_in, 0, sizeof(struct sockaddr_in));
            sa_in->sin_family = AF_INET;
            sa_in->sin_addr = ia;
        }
    }
    if (family == AF_UNSPEC)
    {
        struct in6_addr ia = {};
        if (inet_pton(AF_INET6, addr.c_str(), &ia) == 1)
        {
            family = AF_INET6;
            auto *sa_in = as_sockaddr_in6(sa);
            memset(sa_in, 0, sizeof(struct sockaddr_in6));
            sa_in->sin6_family = AF_INET6;
            sa_in->sin6_addr = ia;
        }
    }
    if (family == AF_UNSPEC)
    {
        struct addrinfo *ai = nullptr;

        if (getaddrinfo(addr.c_str(), nullptr, nullptr, &ai) == 0)
        {
            memcpy(sa, ai->ai_addr, ai->ai_addrlen);
            family = ai->ai_family;
            freeaddrinfo(ai);
        }
    }

    switch (family)
    {
        case AF_INET: {
            auto *sa_in = as_sockaddr_in(sa);
            sa_in->sin_port = htons(port);
        }
        break;
        case AF_INET6: {
            auto *sa_in = as_sockaddr_in6(sa);
            sa_in->sin6_port = htons(port);
        }
        break;
        default: break;
    }

    return family != AF_UNSPEC;
}

void set_tcp_keepalive(int fd, unsigned int tcp_user_timeout_ms) // NOLINT(bugprone-easily-swappable-parameters)
{
    int val = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) < 0)
    {
        FSS_PERROR("transport", "setsockopt SO_KEEPALIVE failed");
    }
    val = 15;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0)
    {
        FSS_PERROR("transport", "setsockopt TCP_KEEPIDLE failed");
    }
    val = 5;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0)
    {
        FSS_PERROR("transport", "setsockopt TCP_KEEPINTVL failed");
    }
    val = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0)
    {
        FSS_PERROR("transport", "setsockopt TCP_KEEPCNT failed");
    }
#ifdef TCP_USER_TIMEOUT
    /* Keepalive probes only fire on an idle connection; once unacked data is
     * in flight the kernel falls back to the retransmission timeout
     * (~15 minutes), so a blocking send() into a black-holed peer can stall
     * a sender thread for that long. TCP_USER_TIMEOUT bounds how long
     * transmitted data may stay unacknowledged before the kernel errors the
     * connection out. The default (default_tcp_user_timeout_ms, 30 s) matches
     * the liveness timeout the server applies at the application layer
     * (default client_timeout); a flight-safety client can pass a tighter
     * per-connection bound (todo/26). */
    /* tcp(7): TCP_USER_TIMEOUT takes an unsigned int (milliseconds). The
     * regression test pins the default with a literal on purpose - changing
     * it must consciously break the test. */
    unsigned int user_timeout_ms = tcp_user_timeout_ms;
    if (setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout_ms, sizeof(user_timeout_ms)) < 0)
    {
        FSS_PERROR("transport", "setsockopt TCP_USER_TIMEOUT failed");
    }
#endif
}
