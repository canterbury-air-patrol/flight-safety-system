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

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "fss-transport.hpp"

using flight_safety_system::transport::fss_connection;
using flight_safety_system::transport::fss_message_identity;

namespace {

/* Build a Unix socketpair and return both fds; close the second so the
 * first observes EOF and the recv thread exits naturally. */
auto make_paired_eof_fd() -> int
{
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
    {
        return -1;
    }
    ::close(fds[1]);
    return fds[0];
}

} // namespace

/* Regression for the C2.3 fix: fss_connection::create() spawns a recv thread
 * inside the static factory; the destructor must safely tear it down. Under
 * tight create/destroy churn TSan must observe no race between the thread
 * startup path and the dtor's disconnect()/join(). */
TEST_CASE("tsan: fss_connection create + immediate destruct over many iterations")
{
    constexpr int iterations = 100;
    for (int i = 0; i < iterations; ++i)
    {
        int fd = make_paired_eof_fd();
        REQUIRE(fd >= 0);
        auto conn = fss_connection::create(fd);
        REQUIRE(conn != nullptr);
        /* Drop the only owning shared_ptr — destructor runs, which calls
         * disconnect() then joins the recv thread. */
    }
}

/* Two threads racing on the same fss_connection: one calling sendMsg in a
 * loop, the other calling disconnect() partway through. The send_lock and
 * fd atomics inside fss_connection must serialize the two operations
 * cleanly — TSan must not report a data race, and the process must not
 * crash or deadlock. */
TEST_CASE("tsan: concurrent sendMsg + disconnect on a connected pair")
{
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    /* Wrap one end in fss_connection; close the other end after a brief
     * delay (handled by the recv thread observing EOF). The peer fd is
     * left open here and closed only at scope exit so writes during the
     * race do not all immediately fail with EPIPE. */
    auto conn = fss_connection::create(fds[0]);
    REQUIRE(conn != nullptr);

    std::atomic<int> send_attempts{0};
    std::atomic<int> send_successes{0};
    std::thread sender([&]() {
        for (int i = 0; i < 200; ++i)
        {
            auto msg = std::make_shared<fss_message_identity>("racer");
            ++send_attempts;
            if (conn->sendMsg(msg))
            {
                ++send_successes;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    /* Let the sender warm up briefly, then yank the connection. */
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    conn->disconnect();

    sender.join();
    /* No crash, no race: the test passes. The exact split between
     * successes and failures depends on scheduling; both are valid. */
    REQUIRE(send_attempts.load() == 200);
    REQUIRE(send_successes.load() <= send_attempts.load());

    ::close(fds[1]);
}
