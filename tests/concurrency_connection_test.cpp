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
using flight_safety_system::transport::fss_message_cb;
using flight_safety_system::transport::fss_message_identity;

namespace {

/* Minimal fss_message_cb subclass for concurrency tests.  Exposes the
 * protected writer methods so the test threads can exercise them directly. */
struct MinimalCb : fss_message_cb {
    explicit MinimalCb(std::shared_ptr<fss_connection> t_conn = nullptr) : fss_message_cb(std::move(t_conn)) {}
    void processMessage(std::shared_ptr<flight_safety_system::transport::fss_message>) override {}
    void testSetConnection(std::shared_ptr<fss_connection> c) { setConnection(std::move(c)); }
    void testClearConnection() { clearConnection(); }
};

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

/* Regression for the thread-safety fix on fss_message_cb::conn: one thread
 * repeatedly writes conn via setConnection/clearConnection while another reads
 * it via sendMsg, getConnection, and connected.  TSan must not report a race. */
TEST_CASE("tsan: concurrent setConnection/clearConnection and sendMsg/getConnection")
{
    auto conn = std::make_shared<fss_connection>();
    MinimalCb cb{conn};
    auto msg = std::make_shared<fss_message_identity>("tsan-cb-racer");

    constexpr int iterations = 5000;
    std::atomic<bool> start{false};

    std::thread writer([&]() {
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        for (int i = 0; i < iterations; ++i)
        {
            cb.testSetConnection(conn);
            cb.testClearConnection();
        }
    });

    std::thread reader([&]() {
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        for (int i = 0; i < iterations; ++i)
        {
            cb.sendMsg(msg);
            cb.getConnection();
            cb.connected();
        }
    });

    start.store(true, std::memory_order_release);
    writer.join();
    reader.join();
}

/* todo/25: processMessage() now runs with msg_lock released, so the delivery
 * bookkeeping (delivery_depth/delivering_thread/delivery_cv) is what serializes
 * deliveries and keeps setHandler() from swapping the handler mid-call. One
 * thread churns setHandler(cb)/setHandler(nullptr) while the recv thread is
 * delivering a steady stream of real messages: TSan must see no race on the
 * handler pointer or the queue, and no delivery may land after a detach
 * returns. */
TEST_CASE("tsan: setHandler churn racing live recv-thread deliveries")
{
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    auto sender = fss_connection::create(fds[0]);
    auto receiver = fss_connection::create(fds[1]);
    REQUIRE(sender != nullptr);
    REQUIRE(receiver != nullptr);

    /* Counts deliveries and asserts none arrives while detached. attached is
     * only written by the churning thread, and only while no delivery can be in
     * flight — setHandler() waits for delivery-idle on both sides of the swap. */
    struct counting_cb : fss_message_cb {
        explicit counting_cb(std::shared_ptr<fss_connection> t_conn) : fss_message_cb(std::move(t_conn)) {}
        std::atomic<bool> attached{false};
        std::atomic<int> delivered{0};
        std::atomic<int> while_detached{0};
        void processMessage(std::shared_ptr<flight_safety_system::transport::fss_message>) override
        {
            if (!this->attached.load())
            {
                ++this->while_detached;
            }
            ++this->delivered;
        }
    };
    auto cb = std::make_shared<counting_cb>(receiver);

    constexpr int iterations = 500;
    std::atomic<bool> stop{false};

    std::thread feeder([&]() {
        while (!stop.load())
        {
            sender->sendMsg(std::make_shared<fss_message_identity>("tsan-churn"));
        }
    });

    for (int i = 0; i < iterations; ++i)
    {
        cb->attached.store(true);
        receiver->setHandler(cb.get());
        receiver->setHandler(nullptr);
        cb->attached.store(false);
        /* Keep the queue from growing without bound while detached. */
        while (receiver->getMsg() != nullptr)
        {
        }
    }

    stop.store(true);
    feeder.join();

    REQUIRE(cb->while_detached.load() == 0);

    cb->disconnect();
    receiver->disconnect();
}
