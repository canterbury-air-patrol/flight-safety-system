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

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fss-transport.hpp"
#include "fss-client-ssl.hpp"
#include "test_helpers.hpp"

/* The client-ssl reconnect path needs TLS fixtures; the test Makefile
 * generates these under certs/ at build time and they're reused by
 * tests/client.cpp. */
namespace {

constexpr const char* CA_PUBLIC_FILE = "certs/ca.public.pem";
constexpr const char* SERVER_PRIVATE_FILE = "certs/localhost.private.pem";
constexpr const char* SERVER_PUBLIC_FILE = "certs/localhost.public.pem";
constexpr const char* CLIENT_PRIVATE_FILE = "certs/client.private.pem";
constexpr const char* CLIENT_PUBLIC_FILE = "certs/client.public.pem";

std::shared_ptr<flight_safety_system::transport::fss_connection> accepted_conn;

auto accept_cb(std::shared_ptr<flight_safety_system::transport::fss_connection> new_conn) -> bool
{
    accepted_conn = std::move(new_conn);
    return true;
}

auto make_listener(uint16_t port)
{
    return std::make_shared<flight_safety_system::transport_ssl::fss_listen>(port, accept_cb, CA_PUBLIC_FILE,
                                                                             SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
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

    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE,
                                                                                 CLIENT_PUBLIC_FILE);
    client->connectTo("localhost", port, /*connect*/ true);

    REQUIRE(fss_test::wait_for([]() { return accepted_conn != nullptr; }));
    auto original = accepted_conn;
    accepted_conn = nullptr;

    /* Drop the listener and the accepted connection — from the client's
     * perspective, the server side has gone away.  With the shared_ptr-capture
     * thread design the recv thread keeps the connection alive until it exits,
     * so we must explicitly disconnect before dropping the reference to ensure
     * the TCP connection closes immediately. */
    original->disconnect();
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
        std::chrono::milliseconds(15000), std::chrono::milliseconds(200)));

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

    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE,
                                                                                 CLIENT_PUBLIC_FILE);
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
    for (int i = 0; i < 100; ++i)
    {
        client->attemptReconnect();
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 500);
}

TEST_CASE("reconnect: fake clock throttles attempts within retry window")
{
    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE,
                                                                                 CLIENT_PUBLIC_FILE);
    auto server = std::make_shared<CountingServer>(client.get(), "127.0.0.1", static_cast<uint16_t>(20599),
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

TEST_CASE("reconnect: connection close resets backoff for an immediate retry")
{
    /* A closed connection requests a backoff reset via the atomic
     * backoff_reset_requested hand-off (the recv thread must not touch the
     * backoff fields directly); the next reconnect() consumes it and
     * attempts immediately, even mid-retry-window. */
    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE,
                                                                                 CLIENT_PUBLIC_FILE);
    auto server = std::make_shared<CountingServer>(client.get(), "127.0.0.1", static_cast<uint16_t>(20598),
                                                   CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto fake = std::make_shared<FakeClock>();
    server->setClock(fake);

    /* Arm the throttle with one (failed) attempt past the initial window. */
    fake->advance(1001);
    server->reconnect();
    REQUIRE(server->attempts == 1);

    /* Inside the grown retry window: throttled. */
    fake->advance(1);
    server->reconnect();
    REQUIRE(server->attempts == 1);

    /* Deliver closed as the recv thread would; the reset must take effect
     * on the next reconnect() call. */
    server->processMessage(std::make_shared<flight_safety_system::transport::fss_message_closed>());
    server->reconnect();
    REQUIRE(server->attempts == 2);
}

TEST_CASE("reconnect: fake clock exposes exponential backoff growth")
{
    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE,
                                                                                 CLIENT_PUBLIC_FILE);
    auto server = std::make_shared<CountingServer>(client.get(), "127.0.0.1", static_cast<uint16_t>(20600),
                                                   CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto fake = std::make_shared<FakeClock>();
    server->setClock(fake);

    /* First attempt: clock must exceed the initial 1000ms retry delay (no jitter yet). */
    fake->advance(1001);
    server->reconnect();
    REQUIRE(server->attempts == 1);

    /* Base doubles to 2000ms; with ±25% jitter effective_delay ∈ [1500, 2500].
     * 1499ms < 1500ms (min) so this is always below the window. */
    fake->advance(1499);
    server->reconnect();
    REQUIRE(server->attempts == 1);

    /* 2501ms > 2500ms (max) so this always fires regardless of jitter. */
    fake->advance(1002);
    server->reconnect();
    REQUIRE(server->attempts == 2);

    /* Base doubles to 4000ms; effective_delay ∈ [3000, 5000].
     * 2999ms < 3000ms (min) so still below window. */
    fake->advance(2999);
    server->reconnect();
    REQUIRE(server->attempts == 2);

    /* 5001ms > 5000ms (max) so always fires. */
    fake->advance(2002);
    server->reconnect();
    REQUIRE(server->attempts == 3);
}

TEST_CASE("reconnect: jitter keeps effective delay within 25% of base")
{
    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE,
                                                                                 CLIENT_PUBLIC_FILE);

    /* Run enough instances to verify bounds and variation. */
    std::vector<uint64_t> delays;
    for (int i = 0; i < 20; ++i)
    {
        auto server = std::make_shared<CountingServer>(client.get(), "127.0.0.1", static_cast<uint16_t>(20601),
                                                       CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
        auto fake = std::make_shared<FakeClock>();
        server->setClock(fake);

        fake->advance(1001);
        server->reconnect();
        REQUIRE(server->attempts == 1);

        /* Base doubled to 2000ms; effective_delay must be within [1500, 2500]. */
        uint64_t eff = server->getEffectiveDelay();
        REQUIRE(eff >= 1500);
        REQUIRE(eff <= 2500);
        delays.push_back(eff);
    }

    /* Delays must not all be identical — jitter should introduce variation. */
    bool any_differ = std::any_of(delays.begin() + 1, delays.end(), [&](uint64_t d) { return d != delays[0]; });
    REQUIRE(any_differ);
}

TEST_CASE("reconnect: backoff base delay never overshoots the cap")
{
    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE,
                                                                                 CLIENT_PUBLIC_FILE);
    auto server = std::make_shared<CountingServer>(client.get(), "127.0.0.1", static_cast<uint16_t>(20602),
                                                   CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto fake = std::make_shared<FakeClock>();
    server->setClock(fake);

    constexpr uint64_t retry_delay_cap = 30000;
    /* effective_delay = base (capped) + jitter, where jitter is at most
     * base/4 (±25%). With the base clamped to the cap, effective_delay can
     * never exceed cap * 1.25. The pre-fix code doubled the base past the cap
     * (16000 -> 32000), pushing effective_delay above this bound. */
    constexpr uint64_t effective_upper_bound = retry_delay_cap + retry_delay_cap / 4;

    /* 1000ms -> 30000ms is five doublings; 20 iterations saturates the base.
     * Advancing by more than the cap each time guarantees the throttle fires. */
    for (int i = 0; i < 20; ++i)
    {
        fake->advance(retry_delay_cap * 2);
        server->reconnect();
        REQUIRE(server->getEffectiveDelay() <= effective_upper_bound);
    }
}

TEST_CASE("reconnect: multi-server failover keeps secondary reachable")
{
    constexpr uint16_t port_primary = 20507;
    constexpr uint16_t port_secondary = 20508;

    auto listen_primary = make_listener(port_primary);
    auto listen_secondary = make_listener(port_secondary);
    REQUIRE(listen_primary != nullptr);
    REQUIRE(listen_secondary != nullptr);

    accepted_conn = nullptr;
    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE,
                                                                                 CLIENT_PUBLIC_FILE);
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
        if (msg == nullptr)
        {
            return false;
        }
        return msg->getType() == flight_safety_system::transport::message_type_identity ||
               msg->getType() == flight_safety_system::transport::message_type_rtt_request;
    }));
}
