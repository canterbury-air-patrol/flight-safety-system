#include <string>
#include <cstdint>
#include <sys/socket.h>
#include <netinet/in.h>

auto convert_str_to_sa(const std::string &addr, uint16_t port, struct sockaddr_storage *sa) -> bool;
void set_tcp_keepalive(int fd);

template<typename T>
inline auto as_sockaddr(T *addr) -> struct sockaddr *
{
    return reinterpret_cast<struct sockaddr *>(addr); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}
inline auto as_sockaddr_in(struct sockaddr_storage *ss) -> struct sockaddr_in *
{
    return reinterpret_cast<struct sockaddr_in *>(ss); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}
inline auto as_sockaddr_in6(struct sockaddr_storage *ss) -> struct sockaddr_in6 *
{
    return reinterpret_cast<struct sockaddr_in6 *>(ss); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}
