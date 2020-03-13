#include <string>
#include <cstring>

#include "transport.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

bool
convert_str_to_sa(std::string addr, uint16_t port, struct sockaddr_storage *sa)
{
    int family = AF_UNSPEC;
    /* Try converting an IP(v4) address first */
    if (family == AF_UNSPEC)
    {
        struct in_addr ia;
        if (inet_pton(AF_INET, addr.c_str(), &ia) == 1)
        {
            family = AF_INET;
            struct sockaddr_in *sa_in = (struct sockaddr_in *)sa;
            memset(sa_in, 0, sizeof(struct sockaddr_in));
            sa_in->sin_family = AF_INET;
            sa_in->sin_addr = ia;
        }
    }
    /* Try converting an IPv6 address */
    if (family == AF_UNSPEC)
    {
        struct in6_addr ia;
        if (inet_pton (AF_INET6, addr.c_str(), &ia) == 1)
        {
            family = AF_INET6;
            struct sockaddr_in6 *sa_in = (struct sockaddr_in6 *)sa;
            memset(sa_in, 0, sizeof(struct sockaddr_in6));
            sa_in->sin6_family = AF_INET6;
            sa_in->sin6_addr = ia;
        }
    }
    /* Use host name lookup (probably DNS) to resolve the name */
    if (family == AF_UNSPEC)
    {
        struct addrinfo *ai = nullptr;
        
        if (getaddrinfo(addr.c_str(), nullptr, nullptr, &ai) == 0)
        {
            memcpy (sa, ai->ai_addr, ai->ai_addrlen);
            family = ai->ai_family;
        }
        
        freeaddrinfo(ai);
    }

    switch (family)
    {
        case AF_INET:
        {
            struct sockaddr_in *sa_in = (struct sockaddr_in *)sa;
            sa_in->sin_port = ntohs (port);
        } break;
        case AF_INET6:
        {
            struct sockaddr_in6 *sa_in = (struct sockaddr_in6 *)sa;
            sa_in->sin6_port = ntohs (port);
        }
    }
    
    return family != AF_UNSPEC;
}
