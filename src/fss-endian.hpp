#pragma once

#include <cstdint>
#include <arpa/inet.h>
#if !defined(__APPLE__)
#include <endian.h>
#endif

namespace flight_safety_system {

inline auto fss_htobe16(uint16_t x) -> uint16_t { return htons(x); }
inline auto fss_be16toh(uint16_t x) -> uint16_t { return ntohs(x); }
inline auto fss_htobe32(uint32_t x) -> uint32_t { return htonl(x); }
inline auto fss_be32toh(uint32_t x) -> uint32_t { return ntohl(x); }

#if defined(__APPLE__)
inline auto fss_htobe64(uint64_t x) -> uint64_t { return htonll(x); }
inline auto fss_be64toh(uint64_t x) -> uint64_t { return ntohll(x); }
#else
inline auto fss_htobe64(uint64_t x) -> uint64_t { return htobe64(x); }
inline auto fss_be64toh(uint64_t x) -> uint64_t { return be64toh(x); }
#endif

} // namespace flight_safety_system
