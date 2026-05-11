#ifdef HAVE_CATCH2_CATCH_ALL_HPP
#include <catch2/catch_all.hpp>
#elif HAVE_CATCH2_CATCH_HPP
#include <catch2/catch.hpp>
#elif HAVE_CATCH_CATCH_HPP
#include <catch/catch.hpp>
#elif HAVE_CATCH_HPP
#include <catch.hpp>
#else
#error No catch header
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <memory>

#include "fss-transport.hpp"
#include "test_helpers.hpp"

using flight_safety_system::transport::fss_connection;
using flight_safety_system::transport::fss_listen;
using flight_safety_system::transport::fss_message;
using flight_safety_system::transport::message_type_closed;

namespace {

std::shared_ptr<fss_connection> accepted_oversized;

auto accept_oversized_cb(std::shared_ptr<fss_connection> new_conn) -> bool
{
    accepted_oversized = std::move(new_conn);
    return true;
}

auto raw_connect_oversized(uint16_t port) -> int
{
    int s = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) { return -1; }
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    ::inet_pton(AF_INET6, "::1", &addr.sin6_addr);
    if (::connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        ::close(s);
        return -1;
    }
    return s;
}

} // namespace

TEST_CASE("negative: oversized declared length closes connection")
{
    accepted_oversized = nullptr;
    uint16_t port = fss_test::pick_port();
    REQUIRE(port != 0);
    auto listen = std::make_shared<fss_listen>(port, accept_oversized_cb);
    REQUIRE(listen != nullptr);

    int fd = raw_connect_oversized(port);
    REQUIRE(fd >= 0);

    REQUIRE(fss_test::wait_for([]() { return accepted_oversized != nullptr; }));

    /* Declare 0xFFFF bytes — well over FSS_MAX_MESSAGE_BYTES. */
    const uint8_t header[2] = {0xFF, 0xFF};
    REQUIRE(::send(fd, header, sizeof(header), 0) == static_cast<ssize_t>(sizeof(header)));

    std::shared_ptr<fss_message> msg;
    REQUIRE(fss_test::wait_for([&]() {
        msg = accepted_oversized->getMsg();
        return msg != nullptr;
    }));
    REQUIRE(msg->getType() == message_type_closed);

    /* Server must have closed its side; client recv returns 0. */
    uint8_t buf[1];
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    REQUIRE(n == 0);

    ::close(fd);
    accepted_oversized = nullptr;
}
