#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "fss-transport.hpp"

namespace fss_test {

inline auto wait_for(const std::function<bool()> &pred,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(2000),
                     std::chrono::milliseconds poll = std::chrono::milliseconds(10)) -> bool
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred()) { return true; }
        std::this_thread::sleep_for(poll);
    }
    return pred();
}

/* Obtain a free ephemeral TCP port by binding to port 0, reading the assigned
 * port, then closing. Caller accepts the TOCTOU risk (another process can grab
 * the port before we rebind) — acceptable for tests. */
inline auto pick_port() -> uint16_t
{
    int sock = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) { return 0; }
    struct sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = 0;
    if (::bind(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        ::close(sock);
        return 0;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(sock, reinterpret_cast<struct sockaddr *>(&addr), &len) < 0)
    {
        ::close(sock);
        return 0;
    }
    uint16_t port = ntohs(addr.sin6_port);
    ::close(sock);
    return port;
}

/* RAII redirect of std::cerr to an internal ostringstream; log output lands
 * there for the duration of the object's lifetime. */
class capture_cerr {
private:
    std::ostringstream sink{};
    std::streambuf *original;
public:
    capture_cerr() : original(std::cerr.rdbuf(sink.rdbuf())) {}
    capture_cerr(const capture_cerr &) = delete;
    capture_cerr(capture_cerr &&) = delete;
    auto operator=(const capture_cerr &) -> capture_cerr & = delete;
    auto operator=(capture_cerr &&) -> capture_cerr & = delete;
    ~capture_cerr() { std::cerr.rdbuf(original); }
    auto str() const -> std::string { return sink.str(); }
    void clear() { sink.str(""); sink.clear(); }
};

/* Build a buf_len with a valid header (length/type/id) but a caller-specified
 * payload and a caller-specified *declared* length field — allowing tests to
 * construct malformed messages (truncated, oversized-length, unknown-type). */
inline auto make_framed_buffer(uint16_t type, uint64_t msg_id,
                               const std::string &payload,
                               uint16_t declared_length)
    -> std::shared_ptr<flight_safety_system::transport::buf_len>
{
    auto bl = std::make_shared<flight_safety_system::transport::buf_len>();
    uint16_t declared_n = htons(declared_length);
    uint16_t type_n = htons(type);
    uint64_t id_n;
    /* htonll emulation: big-endian 64-bit */
    uint8_t id_bytes[sizeof(uint64_t)];
    for (size_t i = 0; i < sizeof(uint64_t); ++i)
    {
        id_bytes[sizeof(uint64_t) - 1 - i] = static_cast<uint8_t>((msg_id >> (i * 8)) & 0xFFU);
    }
    std::memcpy(&id_n, id_bytes, sizeof(uint64_t));
    bl->addData(reinterpret_cast<const char *>(&declared_n), sizeof(uint16_t));
    bl->addData(reinterpret_cast<const char *>(&type_n), sizeof(uint16_t));
    bl->addData(reinterpret_cast<const char *>(&id_n), sizeof(uint64_t));
    if (!payload.empty())
    {
        bl->addData(payload.data(), static_cast<uint16_t>(payload.size()));
    }
    return bl;
}

} // namespace fss_test
