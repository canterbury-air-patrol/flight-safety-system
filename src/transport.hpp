#include <string>
#include <cstdint>
#include <sys/socket.h>
#include <netinet/in.h>

auto convert_str_to_sa(const std::string &addr, uint16_t port, struct sockaddr_storage *sa) -> bool;
/* tcp_user_timeout_ms is the TCP_USER_TIMEOUT to apply (todo/26); pass
 * flight_safety_system::transport::default_tcp_user_timeout_ms unless the
 * connection requested its own bound. No default parameter on purpose —
 * every call site states which bound it is applying. */
void set_tcp_keepalive(int fd, unsigned int tcp_user_timeout_ms); // NOLINT(bugprone-easily-swappable-parameters)
/* close()/shutdown() with EINTR retry and failure logging; context names the
 * call site in the log. shutdown treats ENOTCONN as success. */
auto safe_close_fd(int fd, const char *context) -> int;
auto safe_shutdown_fd(int fd, const char *context) -> int;

template<typename T> inline auto as_sockaddr(T *addr) -> struct sockaddr *
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
