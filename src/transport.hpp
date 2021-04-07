#include <string>
#if __APPLE__
/* Apple already has these defines */
#else
#include <endian.h>
#include <cstdint>
static inline auto ntohll(uint64_t x) -> uint64_t { return be64toh(x); }
static inline auto htonll(uint64_t x) -> uint64_t { return htobe64(x); }
#endif

auto convert_str_to_sa(std::string addr, uint16_t port, struct sockaddr_storage *sa) -> bool;
