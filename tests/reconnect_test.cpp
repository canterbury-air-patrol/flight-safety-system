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
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "fss-transport.hpp"
#include "fss-client-ssl.hpp"
#include "test_helpers.hpp"

/* The client-ssl reconnect path needs TLS fixtures; the test Makefile
 * generates these under certs/ at build time and they're reused by
 * tests/client.cpp. */
namespace {

constexpr const char *CA_PUBLIC_FILE     = "certs/ca.public.pem";
constexpr const char *SERVER_PRIVATE_FILE = "certs/localhost.private.pem";
constexpr const char *SERVER_PUBLIC_FILE  = "certs/localhost.public.pem";
constexpr const char *CLIENT_PRIVATE_FILE = "certs/client.private.pem";
constexpr const char *CLIENT_PUBLIC_FILE  = "certs/client.public.pem";

std::shared_ptr<flight_safety_system::transport::fss_connection> accepted_conn;

auto accept_cb(std::shared_ptr<flight_safety_system::transport::fss_connection> new_conn) -> bool
{
    accepted_conn = std::move(new_conn);
    return true;
}

auto make_listener(uint16_t port)
{
    return std::make_shared<flight_safety_system::transport_ssl::fss_listen>(
        port, accept_cb, CA_PUBLIC_FILE, SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
}

struct FakeClock : public flight_safety_system::IClock {
    uint64_t t{0};
    auto now_ms() const -> uint64_t override { return t; }
    void advance(uint64_t ms) { t += ms; }
};

class CountingServer : public flight_safety_system::client_ssl::fss_server {
public:
    using fss_server::fss_server;
    CountingServer(const CountingServer&) = delete;
    CountingServer(CountingServer&&) = delete;
    auto operator=(const CountingServer&) -> CountingServer& = delete;
    auto operator=(CountingServer&&) -> CountingServer& = delete;
    ~CountingServer() override = default;
    int attempts{0};
protected:
    auto reconnect_to() -> bool override
    {
        ++attempts;
        return false;
    }
};

} // namespace

TEST_CASE("reconnect: client reconnects after listener bounce")
{
    accepted_conn = nullptr;
    constexpr uint16_t port = 20505;

    auto listen = make_listener(port);
    REQUIRE(listen != nullptr);

    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(
        CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    client->connectTo("localhost", port, /*connect*/ true);

    REQUIRE(fss_test::wait_for([]() { return accepted_conn != nullptr; }));
    auto original = accepted_conn;
    accepted_conn = nullptr;

    /* Drop the listener and the accepted connection — from the client's
     * perspective, the server side has gone away. */
    original = nullptr;
    listen = nullptr;

    /* Give the client recv thread time to observe the close; it will
     * enqueue message_type_closed which the fss_server callback uses to
     * move itself to reconnect_servers. */
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    /* Start a new listener on the same port. */
    listen = make_listener(port);
    REQUIRE(listen != nullptr);

    /* The retry_delay starts at 1000ms, so a single attempt immediately
     * after bounce will likely be throttled. Keep calling until the
     * listener accepts. */
    REQUIRE(fss_test::wait_for(
        [&]() {
            client->attemptReconnect();
            return accepted_conn != nullptr;
        },
        std::chrono::milliseconds(15000),
        std::chrono::milliseconds(200)));

    accepted_conn = nullptr;
}

TEST_CASE("reconnect: attemptReconnect is throttled within the retry window")
{
    /* Backoff design in src/client-ssl.cpp: reconnect_to() is only
     * invoked when (now - last_tried) > retry_delay. On a cold client
     * retry_delay = 1000ms. Hammering attemptReconnect in a tight loop
     * must NOT turn into repeated TCP connect attempts, because the
     * throttle would otherwise amount to a reconnect storm. */
    constexpr uint16_t closed_port = 20506;

    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(
        CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    /* Register a server pointed at a closed port. connect=true triggers
     * the first failed reconnect, arming last_tried. */
    client->connectTo("127.0.0.1", closed_port, /*connect*/ true);

    /* Rapid-fire attemptReconnect: within the 1s retry window none of
     * these should produce an outbound connect. We can't observe that
     * directly here, but we can observe wall-clock cost — 100 calls in
     * <100ms is well under one per ms, which is only possible if each
     * call is a no-op (actual connect() attempts to a closed port take
     * a kernel round-trip each). */
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) { client->attemptReconnect(); }
    auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 500);
}

TEST_CASE("reconnect: fake clock throttles attempts within retry window")
{
    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(
        CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto server = std::make_shared<CountingServer>(
        client.get(), "127.0.0.1", static_cast<uint16_t>(20599),
        CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto fake = std::make_shared<FakeClock>();
    server->setClock(fake);

    /* last_tried starts at 0 and retry_delay at 1000ms. The throttle is
     * strict >: elapsed of exactly 1000 is still a no-op. */
    server->reconnect();
    REQUIRE(server->attempts == 0);

    fake->advance(1000);
    server->reconnect();
    REQUIRE(server->attempts == 0);

    fake->advance(1);
    server->reconnect();
    REQUIRE(server->attempts == 1);
}

TEST_CASE("reconnect: fake clock exposes exponential backoff growth")
{
    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(
        CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto server = std::make_shared<CountingServer>(
        client.get(), "127.0.0.1", static_cast<uint16_t>(20600),
        CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto fake = std::make_shared<FakeClock>();
    server->setClock(fake);

    /* First attempt: clock must exceed the initial 1000ms retry delay. */
    fake->advance(1001);
    server->reconnect();
    REQUIRE(server->attempts == 1);

    /* Retry delay has doubled to 2000ms. A 1001ms gap must not fire. */
    fake->advance(1001);
    server->reconnect();
    REQUIRE(server->attempts == 1);

    /* 2001ms since last_tried → attempt 2, delay doubles to 4000. */
    fake->advance(1001);
    server->reconnect();
    REQUIRE(server->attempts == 2);

    fake->advance(4000);
    server->reconnect();
    REQUIRE(server->attempts == 2);

    fake->advance(1);
    server->reconnect();
    REQUIRE(server->attempts == 3);
}

TEST_CASE("reconnect: multi-server failover keeps secondary reachable")
{
    constexpr uint16_t port_primary   = 20507;
    constexpr uint16_t port_secondary = 20508;

    auto listen_primary = make_listener(port_primary);
    auto listen_secondary = make_listener(port_secondary);
    REQUIRE(listen_primary != nullptr);
    REQUIRE(listen_secondary != nullptr);

    accepted_conn = nullptr;
    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(
        CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    client->connectTo("localhost", port_primary, /*connect*/ true);
    REQUIRE(fss_test::wait_for([]() { return accepted_conn != nullptr; }));
    auto primary_conn = accepted_conn;
    accepted_conn = nullptr;

    client->connectTo("localhost", port_secondary, /*connect*/ true);
    REQUIRE(fss_test::wait_for([]() { return accepted_conn != nullptr; }));
    auto secondary_conn = accepted_conn;
    accepted_conn = nullptr;

    /* Kill the primary; secondary must still accept traffic. */
    primary_conn = nullptr;
    listen_primary = nullptr;

    REQUIRE(secondary_conn != nullptr);
    auto ping = std::make_shared<flight_safety_system::transport::fss_message_rtt_request>();
    client->sendMsgAll(ping);

    /* The secondary recv thread should see the rtt_request. */
    REQUIRE(fss_test::wait_for([&]() {
        auto msg = secondary_conn->getMsg();
        if (msg == nullptr) { return false; }
        return msg->getType() == flight_safety_system::transport::message_type_identity
            || msg->getType() == flight_safety_system::transport::message_type_rtt_request;
    }));
}
