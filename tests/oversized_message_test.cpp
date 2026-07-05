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

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "fss-transport.hpp"
#include "secure-string.hpp"
#include "test_helpers.hpp"

using flight_safety_system::secure_string;
using flight_safety_system::transport::fss_connection;
using flight_safety_system::transport::fss_listen;
using flight_safety_system::transport::fss_message;
using flight_safety_system::transport::fss_message_rtt_request;
using flight_safety_system::transport::fss_message_smm_settings;
using flight_safety_system::transport::message_type_closed;

namespace {

/* The listener's accept callback runs on its worker thread; hand the accepted
 * connection to the main thread through the mutex-guarded handoff. */
fss_test::connection_handoff handoff;

auto raw_connect_oversized(uint16_t port) -> int
{
    int s = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0)
    {
        return -1;
    }
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
    handoff.reset();
    uint16_t port = fss_test::pick_port();
    REQUIRE(port != 0);
    auto listen = std::make_shared<fss_listen>(port, handoff.callback());
    REQUIRE(listen != nullptr);

    int fd = raw_connect_oversized(port);
    REQUIRE(fd >= 0);

    auto accepted_oversized = handoff.wait();
    REQUIRE(accepted_oversized != nullptr);

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
    handoff.reset();
}

TEST_CASE("negative: an oversized message fails the send without consuming a sequence id")
{
    /* todo/38: a packed message over the 16-bit length field used to still
     * transmit unframed (updateSize() left the length placeholder at 0 and
     * the caller only checked isValid(), which merely means "non-empty").
     * The receiver read length 0 and then parsed the remaining ~180 KB
     * payload as a stream of garbage frame headers. sendMsg() must now fail
     * the send outright, and the failed attempt must not consume a
     * per-connection message id — otherwise the gap left behind would look
     * like an out-of-order message to a v2 sequence check (todo/39). */
    handoff.reset();
    uint16_t port = fss_test::pick_port();
    REQUIRE(port != 0);
    auto listen = std::make_shared<fss_listen>(port, handoff.callback());
    REQUIRE(listen != nullptr);

    auto conn = std::make_shared<fss_connection>();
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("localhost", port));

    auto server_conn = handoff.wait();
    REQUIRE(server_conn != nullptr);

    auto get_next = [&]() -> std::shared_ptr<fss_message> {
        std::shared_ptr<fss_message> msg;
        REQUIRE(fss_test::wait_for([&]() -> bool {
            msg = server_conn->getMsg();
            return msg != nullptr;
        }));
        return msg;
    };

    REQUIRE(conn->sendMsg(std::make_shared<fss_message_rtt_request>()));
    uint64_t first_id = get_next()->getId();

    /* Three ~60 KB strings, each individually under packStringRaw's 16-bit
     * per-string clamp, but packing to ~180 KB together — well over the
     * whole-message 16-bit length field. */
    std::string huge(60000, 'x');
    auto oversized = std::make_shared<fss_message_smm_settings>(huge, secure_string(huge), secure_string(huge));
    REQUIRE_FALSE(conn->sendMsg(oversized));

    /* Nothing from the failed send reaches the peer. */
    REQUIRE_FALSE(
        fss_test::wait_for([&]() -> bool { return server_conn->getMsg() != nullptr; }, std::chrono::milliseconds(200)));

    REQUIRE(conn->sendMsg(std::make_shared<fss_message_rtt_request>()));
    uint64_t second_id = get_next()->getId();

    REQUIRE(second_id == first_id + 1);

    handoff.reset();
}
