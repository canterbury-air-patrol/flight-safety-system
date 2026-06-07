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

#include <atomic>
#include <memory>
#include <stdexcept>

#include "fss-transport.hpp"
#include "test_helpers.hpp"

/* Verify that a handler whose processMessage() throws does NOT kill the
 * recv thread: the loop should catch the exception, log it, and continue
 * processing subsequent messages. */

namespace {

/* A handler that throws std::runtime_error on the first call, then records
 * all subsequent messages so we can assert the loop survived. */
class throwing_message_cb : public flight_safety_system::transport::fss_message_cb {
public:
    std::atomic<int> call_count{0};
    std::atomic<int> after_throw_count{0};

    explicit throwing_message_cb(std::shared_ptr<flight_safety_system::transport::fss_connection> t_conn)
        : fss_message_cb(std::move(t_conn))
    {
    }
    throwing_message_cb(const throwing_message_cb &) = delete;
    throwing_message_cb(throwing_message_cb &&) = delete;
    auto operator=(const throwing_message_cb &) -> throwing_message_cb & = delete;
    auto operator=(throwing_message_cb &&) -> throwing_message_cb & = delete;
    ~throwing_message_cb() override = default;

    void processMessage(std::shared_ptr<flight_safety_system::transport::fss_message> /*message*/) override
    {
        int n = ++call_count;
        if (n == 1)
        {
            throw std::runtime_error("deliberate test exception from processMessage");
        }
        ++after_throw_count;
    }
};

static std::shared_ptr<flight_safety_system::transport::fss_connection> exception_test_conn;

auto exception_test_accept_cb(std::shared_ptr<flight_safety_system::transport::fss_connection> new_conn) -> bool
{
    exception_test_conn = std::move(new_conn);
    return true;
}

} // namespace

TEST_CASE("processMessages: throwing handler does not kill recv loop")
{
    exception_test_conn = nullptr;
    const auto port = fss_test::pick_port();
    REQUIRE(port != 0);

    auto listen = std::make_shared<flight_safety_system::transport::fss_listen>(port, exception_test_accept_cb);
    REQUIRE(listen != nullptr);

    auto sender = std::make_shared<flight_safety_system::transport::fss_connection>();
    REQUIRE(sender != nullptr);
    REQUIRE(sender->connectTo("localhost", port));

    REQUIRE(fss_test::wait_for([]() { return exception_test_conn != nullptr; }));

    auto cb = std::make_shared<throwing_message_cb>(exception_test_conn);
    exception_test_conn->setHandler(cb.get());

    /* First message — handler throws; loop must survive. */
    sender->sendMsg(std::make_shared<flight_safety_system::transport::fss_message_rtt_request>());
    REQUIRE(fss_test::wait_for([&]() { return cb->call_count.load() >= 1; }));

    /* Second message — loop must still be running and dispatch it. */
    sender->sendMsg(std::make_shared<flight_safety_system::transport::fss_message_rtt_request>());
    REQUIRE(fss_test::wait_for([&]() { return cb->after_throw_count.load() >= 1; }));

    REQUIRE(cb->call_count.load() >= 2);

    cb->disconnect();
    sender = nullptr;
    exception_test_conn = nullptr;
}
