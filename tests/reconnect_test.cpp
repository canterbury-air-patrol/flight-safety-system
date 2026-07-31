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
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include "fss-transport.hpp"
#include "fss-client-ssl.hpp"
#include "test_helpers.hpp"

/* Shared controllable clock (tests/test_helpers.hpp). */
using fss_test::FakeClock;

/* The client-ssl reconnect path needs TLS fixtures; the test Makefile
 * generates these under certs/ at build time and they're reused by
 * tests/client.cpp. */
namespace {

constexpr const char* CA_PUBLIC_FILE = "certs/ca.public.pem";
constexpr const char* SERVER_PRIVATE_FILE = "certs/localhost.private.pem";
constexpr const char* SERVER_PUBLIC_FILE = "certs/localhost.public.pem";
constexpr const char* CLIENT_PRIVATE_FILE = "certs/client.private.pem";
constexpr const char* CLIENT_PUBLIC_FILE = "certs/client.public.pem";

/* The listener runs its accept callback on its own worker thread; the handoff
 * publishes the accepted connection to the main thread with a happens-before
 * edge. reset() between connections lets a test wait for the *next* accept. */
fss_test::connection_handoff handoff;

auto make_listener(uint16_t port)
{
    return std::make_shared<flight_safety_system::transport_ssl::fss_listen>(port, handoff.callback(), CA_PUBLIC_FILE,
                                                                             SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
}

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

/* Replacement connection that "sends" successfully without any socket. */
class NullConnection : public flight_safety_system::transport::fss_connection {
public:
    NullConnection() = default;
    NullConnection(const NullConnection&) = delete;
    NullConnection(NullConnection&&) = delete;
    auto operator=(const NullConnection&) -> NullConnection& = delete;
    auto operator=(NullConnection&&) -> NullConnection& = delete;
    ~NullConnection() override = default;
protected:
    auto sendMsg(const std::shared_ptr<flight_safety_system::transport::buf_len>&) -> bool override { return true; }
};

/* A server that can adopt an externally built connection (standing in for
 * the live one a liveness trip is about to replace) and whose redial always
 * succeeds by installing a NullConnection. */
class SwapServer : public flight_safety_system::client_ssl::fss_server {
public:
    using fss_server::fss_server;
    SwapServer(const SwapServer&) = delete;
    SwapServer(SwapServer&&) = delete;
    auto operator=(const SwapServer&) -> SwapServer& = delete;
    auto operator=(SwapServer&&) -> SwapServer& = delete;
    ~SwapServer() override = default;
    auto connected() -> bool override { return true; }
    void adopt(const std::shared_ptr<flight_safety_system::transport::fss_connection>& t_conn)
    {
        this->setConnection(t_conn);
    }
protected:
    auto reconnect_to() -> bool override
    {
        this->setConnection(std::make_shared<NullConnection>());
        return true;
    }
};

/* A client that counts serverRequiresReconnect calls, to detect a stale
 * connection's closed event being misattributed to a healthy server. */
class ObservingClient : public flight_safety_system::client_ssl::fss_client {
public:
    using fss_client::fss_client;
    ObservingClient(const ObservingClient&) = delete;
    ObservingClient(ObservingClient&&) = delete;
    auto operator=(const ObservingClient&) -> ObservingClient& = delete;
    auto operator=(ObservingClient&&) -> ObservingClient& = delete;
    ~ObservingClient() override = default;
    std::atomic<int> reconnect_requests{0};
    void serverRequiresReconnect(flight_safety_system::client_ssl::fss_server* server) override
    {
        ++this->reconnect_requests;
        fss_client::serverRequiresReconnect(server);
    }
    void add(const std::shared_ptr<flight_safety_system::client_ssl::fss_server>& server) { this->addServer(server); }
};

} // namespace

TEST_CASE("reconnect: client reconnects after listener bounce")
{
    handoff.reset();
    constexpr uint16_t port = 20505;

    auto listen = make_listener(port);
    REQUIRE(listen != nullptr);

    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE,
                                                                                 CLIENT_PUBLIC_FILE);
    client->connectTo("localhost", port, /*connect*/ true);

    auto original = handoff.wait();
    REQUIRE(original != nullptr);
    handoff.reset();

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
            return handoff.get() != nullptr;
        },
        std::chrono::milliseconds(15000), std::chrono::milliseconds(200)));

    handoff.reset();
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
     * these should produce an outbound connect. Since todo/66 the dial runs
     * on the server's own worker, so the caller's thread is cheap by
     * construction and this only holds the *caller* side to being a no-op —
     * that the dialling itself stays throttled is asserted directly by the
     * fake-clock case below (which counts reconnect_to calls) and by
     * "repeated attemptReconnect does not pile up dials on a stalled server"
     * in client_fanout_test.cpp (which counts queued attempts). */
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

    handoff.reset();
    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE,
                                                                                 CLIENT_PUBLIC_FILE);
    client->connectTo("localhost", port_primary, /*connect*/ true);
    auto primary_conn = handoff.wait();
    REQUIRE(primary_conn != nullptr);
    handoff.reset();

    client->connectTo("localhost", port_secondary, /*connect*/ true);
    auto secondary_conn = handoff.wait();
    REQUIRE(secondary_conn != nullptr);
    handoff.reset();

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

TEST_CASE("reconnect: reconnect() retires the old connection instead of leaking it")
{
    /* todo/65: reconnect() used to drop its reference to the previous
     * connection without disconnecting it. The recv-thread lambda holds the
     * connection shared_ptr, so the "discarded" connection stayed fully
     * alive — fd open, recv thread running — leaking a thread + fd per
     * liveness-triggered reconnect and leaving the old session live at the
     * server, which then rejects this asset's re-identify as a duplicate. */
    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>();
    auto server = std::make_shared<SwapServer>(client.get(), "127.0.0.1", static_cast<uint16_t>(20603), "", "", "");

    std::array<int, 2> sv{};
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()) == 0);
    auto old_conn = flight_safety_system::transport::fss_connection::create(sv[0]);
    REQUIRE(old_conn != nullptr);
    old_conn->setHandler(server.get());
    server->adopt(old_conn);
    std::weak_ptr<flight_safety_system::transport::fss_connection> retired = old_conn;
    old_conn = nullptr;

    REQUIRE(server->reconnect());
    REQUIRE(server->connected());

    /* The recv thread held the last reference; only a real disconnect (which
     * joins that thread) lets the old connection actually die. */
    REQUIRE(retired.expired());

    close(sv[1]);
}

TEST_CASE("reconnect: a retired connection's closed event cannot tear down the replacement")
{
    /* todo/65: the old connection's handler used to stay wired to the
     * fss_server across reconnect(), so when the old connection finally died
     * its closed event hit serverRequiresReconnect() and yanked the server —
     * with its healthy replacement connection — back into the reconnect
     * queue, where the next tick would tear the replacement down too. */
    auto client = std::make_shared<ObservingClient>();
    auto server = std::make_shared<SwapServer>(client.get(), "127.0.0.1", static_cast<uint16_t>(20604), "", "", "");

    std::array<int, 2> sv{};
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()) == 0);
    auto old_conn = flight_safety_system::transport::fss_connection::create(sv[0]);
    REQUIRE(old_conn != nullptr);
    old_conn->setHandler(server.get());
    server->adopt(old_conn);
    old_conn = nullptr;
    client->add(server);

    REQUIRE(server->reconnect());
    REQUIRE(client->reconnect_requests.load() == 0);

    /* Sever the far end of the retired connection. Pre-fix, its still-wired
     * recv thread delivered message_type_closed to the fss_server; with the
     * handler detached before disconnect there is no thread left to deliver
     * anything, so the replacement must stay untouched. */
    close(sv[1]);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(client->reconnect_requests.load() == 0);
}
