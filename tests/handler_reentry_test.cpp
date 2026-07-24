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
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fss-transport.hpp"
#include "test_helpers.hpp"

/* todo/25: fss_connection delivers processMessage() with msg_lock RELEASED, so
 * a handler may re-enter the connection's message-queue API from inside its own
 * callback without deadlocking. Before that change every case below hung the
 * recv thread on a plain std::mutex.
 *
 * Every assertion is a bounded wait_for(), so a regression fails on the timeout
 * rather than wedging the suite at the point of the deadlock. */

using flight_safety_system::transport::fss_connection;
using flight_safety_system::transport::fss_message;
using flight_safety_system::transport::fss_message_cb;
using flight_safety_system::transport::fss_message_identity;

namespace {

/* A handler that runs an arbitrary action from inside processMessage().
 * `entered` is bumped before the action and `returned` after it, so a callback
 * that deadlocks shows up as entered == 1, returned == 0. */
class reentrant_cb : public fss_message_cb {
public:
    using action_fn = std::function<void(reentrant_cb &)>;

    reentrant_cb(std::shared_ptr<fss_connection> t_conn, action_fn t_action)
        : fss_message_cb(std::move(t_conn)), action(std::move(t_action))
    {
    }
    reentrant_cb(const reentrant_cb &) = delete;
    reentrant_cb(reentrant_cb &&) = delete;
    auto operator=(const reentrant_cb &) -> reentrant_cb & = delete;
    auto operator=(reentrant_cb &&) -> reentrant_cb & = delete;
    ~reentrant_cb() override = default;

    std::atomic<int> entered{0};
    std::atomic<int> returned{0};

    void processMessage(std::shared_ptr<fss_message> message) override
    {
        ++this->entered;
        {
            const std::scoped_lock lock(this->names_lock);
            auto ident = std::dynamic_pointer_cast<fss_message_identity>(message);
            this->names.push_back(ident != nullptr ? ident->getName() : std::string("<other>"));
        }
        if (this->action)
        {
            this->action(*this);
        }
        ++this->returned;
    }

    auto getNames() -> std::vector<std::string>
    {
        const std::scoped_lock lock(this->names_lock);
        return this->names;
    }
private:
    action_fn action;
    std::mutex names_lock{};
    std::vector<std::string> names{};
};

/* A connected pair of fss_connections over a Unix socketpair: no listener, no
 * port, and the recv threads are up as soon as create() returns. */
struct conn_pair {
    std::shared_ptr<fss_connection> sender{};
    std::shared_ptr<fss_connection> receiver{};
};

auto make_conn_pair(size_t receiver_queue_size = 1000) -> conn_pair
{
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
    {
        return {};
    }
    conn_pair pair;
    pair.sender = fss_connection::create(fds[0]);
    pair.receiver = fss_connection::create(fds[1], receiver_queue_size);
    return pair;
}

auto send_identity(const std::shared_ptr<fss_connection> &conn, const std::string &name) -> bool
{
    return conn->sendMsg(std::make_shared<fss_message_identity>(name));
}

} // namespace

TEST_CASE("reentry: handler calling getMsg() from processMessage does not deadlock")
{
    auto pair = make_conn_pair();
    REQUIRE(pair.receiver != nullptr);

    std::atomic<bool> got_null{false};
    std::atomic<bool> called_getmsg{false};
    auto cb = std::make_shared<reentrant_cb>(pair.receiver, [&](reentrant_cb &self) -> void {
        /* getMsg() takes msg_lock, which the old code still held here. */
        auto queued = self.getConnection()->getMsg();
        called_getmsg.store(true);
        /* With a handler installed getMsg() always reports empty. */
        got_null.store(queued == nullptr);
    });
    pair.receiver->setHandler(cb.get());

    REQUIRE(send_identity(pair.sender, "m1"));
    REQUIRE(fss_test::wait_for([&]() { return cb->returned.load() >= 1; }));
    REQUIRE(called_getmsg.load());
    REQUIRE(got_null.load());

    cb->disconnect();
}

TEST_CASE("reentry: handler calling setHandler(nullptr) from processMessage does not deadlock")
{
    auto pair = make_conn_pair();
    REQUIRE(pair.receiver != nullptr);

    auto cb = std::make_shared<reentrant_cb>(
        pair.receiver, [](reentrant_cb &self) -> void { self.getConnection()->setHandler(nullptr); });
    pair.receiver->setHandler(cb.get());

    REQUIRE(send_identity(pair.sender, "m1"));
    REQUIRE(fss_test::wait_for([&]() { return cb->returned.load() >= 1; }));

    /* Detached mid-callback: everything after it queues instead of being
     * delivered, and is readable through getMsg() again. */
    REQUIRE(send_identity(pair.sender, "m2"));
    std::shared_ptr<fss_message> queued{};
    REQUIRE(fss_test::wait_for([&]() {
        queued = pair.receiver->getMsg();
        return queued != nullptr;
    }));
    auto ident = std::dynamic_pointer_cast<fss_message_identity>(queued);
    REQUIRE(ident != nullptr);
    REQUIRE(ident->getName() == "m2");
    REQUIRE(cb->entered.load() == 1);

    pair.receiver->disconnect();
}

TEST_CASE("reentry: handler calling detachHandler() from processMessage does not deadlock")
{
    /* Same barrier as setHandler(nullptr) above, through the entry point
     * ~fss_message_cb uses — the one that is callback-free by contract. */
    auto pair = make_conn_pair();
    REQUIRE(pair.receiver != nullptr);

    auto cb = std::make_shared<reentrant_cb>(pair.receiver,
                                             [](reentrant_cb &self) -> void { self.getConnection()->detachHandler(); });
    pair.receiver->setHandler(cb.get());

    REQUIRE(send_identity(pair.sender, "m1"));
    REQUIRE(fss_test::wait_for([&]() { return cb->returned.load() >= 1; }));

    REQUIRE(send_identity(pair.sender, "m2"));
    std::shared_ptr<fss_message> queued{};
    REQUIRE(fss_test::wait_for([&]() {
        queued = pair.receiver->getMsg();
        return queued != nullptr;
    }));
    auto ident = std::dynamic_pointer_cast<fss_message_identity>(queued);
    REQUIRE(ident != nullptr);
    REQUIRE(ident->getName() == "m2");
    REQUIRE(cb->entered.load() == 1);

    pair.receiver->disconnect();
}

TEST_CASE("reentry: handler calling disconnect() from processMessage does not deadlock")
{
    auto pair = make_conn_pair();
    REQUIRE(pair.receiver != nullptr);

    auto cb = std::make_shared<reentrant_cb>(pair.receiver,
                                             [](reentrant_cb &self) -> void { self.getConnection()->disconnect(); });
    pair.receiver->setHandler(cb.get());

    REQUIRE(send_identity(pair.sender, "m1"));
    REQUIRE(fss_test::wait_for([&]() { return cb->returned.load() >= 1; }));
    /* The self-disconnect really took effect: the socket has been shut down and
     * retired, so the connection can no longer send. (connected() only reports
     * whether the callback still holds a connection pointer, which it does.) */
    REQUIRE_FALSE(send_identity(pair.receiver, "after-disconnect"));

    cb->disconnect();
}

TEST_CASE("reentry: setHandler backlog flush stops when the handler detaches mid-flush")
{
    /* max_queue_size 3, four messages sent with no handler installed: the
     * oldest is dropped, which is an observable signal (getDroppedMessages)
     * that all four have been received and exactly three are queued. Without
     * it there is no way to know the backlog is complete before installing the
     * handler. */
    auto pair = make_conn_pair(3);
    REQUIRE(pair.receiver != nullptr);

    for (const auto *name : {"m1", "m2", "m3", "m4"})
    {
        REQUIRE(send_identity(pair.sender, name));
    }
    REQUIRE(fss_test::wait_for([&]() { return pair.receiver->getDroppedMessages() >= 1; }));

    auto cb = std::make_shared<reentrant_cb>(
        pair.receiver, [](reentrant_cb &self) -> void { self.getConnection()->setHandler(nullptr); });
    /* Flushes the backlog on THIS thread; the first message detaches the
     * handler, so the remaining two must stay queued. */
    pair.receiver->setHandler(cb.get());

    REQUIRE(cb->entered.load() == 1);
    REQUIRE(cb->getNames() == std::vector<std::string>{"m2"});

    for (const auto *expected : {"m3", "m4"})
    {
        auto queued = pair.receiver->getMsg();
        REQUIRE(queued != nullptr);
        auto ident = std::dynamic_pointer_cast<fss_message_identity>(queued);
        REQUIRE(ident != nullptr);
        REQUIRE(ident->getName() == expected);
    }
    REQUIRE(pair.receiver->getMsg() == nullptr);

    pair.receiver->disconnect();
}

TEST_CASE("reentry: setHandler(nullptr) waits for an in-flight callback to return")
{
    /* The lifetime proof for fss_connection's raw fss_message_cb*: detaching
     * must not return while a call into the handler is still running, or the
     * handler could be destroyed under the transport's feet. */
    auto pair = make_conn_pair();
    REQUIRE(pair.receiver != nullptr);

    std::mutex block_lock{};
    std::condition_variable block_cv{};
    bool release = false;

    auto cb = std::make_shared<reentrant_cb>(pair.receiver, [&](reentrant_cb & /*self*/) -> void {
        std::unique_lock lock(block_lock);
        block_cv.wait(lock, [&]() { return release; });
    });
    pair.receiver->setHandler(cb.get());

    REQUIRE(send_identity(pair.sender, "m1"));
    REQUIRE(fss_test::wait_for([&]() { return cb->entered.load() >= 1; }));

    std::atomic<bool> detached{false};
    std::thread detacher([&]() {
        pair.receiver->setHandler(nullptr);
        detached.store(true);
    });

    /* The callback is still blocked, so setHandler must be too. */
    REQUIRE_FALSE(fss_test::wait_for([&]() { return detached.load(); }, std::chrono::milliseconds(200)));
    REQUIRE(cb->returned.load() == 0);

    {
        const std::scoped_lock lock(block_lock);
        release = true;
    }
    block_cv.notify_all();

    REQUIRE(fss_test::wait_for([&]() { return detached.load(); }));
    detacher.join();
    REQUIRE(cb->returned.load() == 1);

    pair.receiver->disconnect();
}
