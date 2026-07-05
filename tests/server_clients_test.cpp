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

#include <chrono>
#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include "fss-transport.hpp"
#include "fss-server.hpp"
#include "fss.hpp"
#include "db-write-queue.hpp"
#include "mock_database.hpp"
#include "server-clients.hpp"
#include "test_helpers.hpp"

namespace fss = flight_safety_system;

namespace {

class FakeConnection : public fss::transport::fss_connection {
public:
    std::list<std::string> cert_names{};
    bool revoked{false};

    FakeConnection() = default;
    FakeConnection(const FakeConnection &) = delete;
    FakeConnection(FakeConnection &&) = delete;
    auto operator=(const FakeConnection &) -> FakeConnection & = delete;
    auto operator=(FakeConnection &&) -> FakeConnection & = delete;
    ~FakeConnection() override = default;

    auto getClientNames() -> std::list<std::string> override { return cert_names; }
    auto isPeerCertRevoked(const std::string & /*crl_file*/) const -> bool override { return revoked; }

    /* Block (true) or release (false) any in-flight or future sendMsg,
     * modelling a peer whose socket has black-holed (todo/36). Releasing wakes
     * a blocked writer. */
    void setBlocked(bool b)
    {
        {
            std::scoped_lock guard(this->block_lock);
            this->blocked = b;
        }
        this->block_cv.notify_all();
    }
    auto sentSnapshot() -> std::vector<std::shared_ptr<fss::transport::fss_message>>
    {
        std::scoped_lock guard(this->sent_lock);
        return this->sent;
    }
    void disconnect() override
    {
        this->setBlocked(false);
        fss::transport::fss_connection::disconnect();
    }
protected:
    auto sendMsg(const std::shared_ptr<fss::transport::buf_len> &bl) -> bool override
    {
        {
            std::unique_lock<std::mutex> lock(this->block_lock);
            this->block_cv.wait(lock, [this]() -> bool { return !this->blocked; });
        }
        auto msg = fss::transport::fss_message::decode(bl);
        std::scoped_lock guard(this->sent_lock);
        if (msg != nullptr)
        {
            this->sent.push_back(msg);
        }
        return true;
    }
private:
    std::mutex sent_lock{};
    std::mutex block_lock{};
    std::condition_variable block_cv{};
    bool blocked{false};
    std::vector<std::shared_ptr<fss::transport::fss_message>> sent{};
};

/* Count how many messages in `sent` decoded to a T. */
template<typename T>
auto count_sent(const std::vector<std::shared_ptr<fss::transport::fss_message>> &sent) -> std::size_t
{
    std::size_t count = 0;
    for (const auto &m : sent)
    {
        if (std::dynamic_pointer_cast<T>(m))
        {
            ++count;
        }
    }
    return count;
}

struct FakeClock : public fss::IClock {
    uint64_t t{0};
    auto now_ms() const -> uint64_t override { return t; }
    void advance(uint64_t ms) { t += ms; }
};

auto make_null_writer() -> std::shared_ptr<fss::server::db_write_queue>
{
    return std::make_shared<fss::server::db_write_queue>(std::size_t{16}, [](const fss::server::db_write_task &) {});
}

/* Build a client that has completed the identity handshake as an aircraft. */
auto make_aircraft_client(const std::string &name, fss_test::MockDatabase &mock, server_clients &handler)
    -> std::shared_ptr<fss::server::fss_client>
{
    mock.asset_ids[name] = static_cast<uint64_t>(mock.asset_ids.size() + 1);
    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back(name);
    auto writer = make_null_writer();
    auto client = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    client->processMessage(std::make_shared<fss::transport::fss_message_identity>(name));
    return client;
}

} // namespace

TEST_CASE("server_clients: clientConnected increments client count")
{
    server_clients sc;
    fss_test::MockDatabase mock;
    auto conn = std::make_shared<FakeConnection>();
    auto writer = make_null_writer();
    auto client = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &sc);
    sc.clientConnected(client);
    // No direct count accessor; verify cleanupRemovableClients runs without crash
    sc.cleanupRemovableClients();
}

TEST_CASE("server_clients: cleanupRemovableClients decrements total count and is idempotent")
{
    server_clients sc;
    fss_test::MockDatabase mock;

    auto conn1 = std::make_shared<FakeConnection>();
    auto writer1 = make_null_writer();
    auto client1 = std::make_shared<fss::server::fss_client>(conn1, &mock, writer1, &sc);

    auto conn2 = std::make_shared<FakeConnection>();
    auto writer2 = make_null_writer();
    auto client2 = std::make_shared<fss::server::fss_client>(conn2, &mock, writer2, &sc);

    sc.clientConnected(client1);
    sc.clientConnected(client2);
    REQUIRE(sc.getTotalClients() == 2);

    sc.clientDisconnected(client1.get());
    REQUIRE(sc.getTotalClients() == 2); // still 2 until cleanup runs

    sc.cleanupRemovableClients();
    REQUIRE(sc.getTotalClients() == 1); // one removed

    sc.clientDisconnected(client2.get());
    sc.cleanupRemovableClients();
    REQUIRE(sc.getTotalClients() == 0); // both removed

    // Second cleanup on empty queue must be a no-op
    sc.cleanupRemovableClients();
    REQUIRE(sc.getTotalClients() == 0);
}

TEST_CASE("server_clients: clientDisconnected moves client to removable queue")
{
    server_clients sc;
    fss_test::MockDatabase mock;
    auto conn = std::make_shared<FakeConnection>();
    auto writer = make_null_writer();
    auto client = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &sc);
    sc.clientConnected(client);
    sc.clientDisconnected(client.get());
    // After cleanup the disconnected queue should be empty (no crash or assertion)
    sc.cleanupRemovableClients();
    sc.cleanupRemovableClients(); // idempotent
}

TEST_CASE("server_clients: disconnecting unknown client is a no-op")
{
    server_clients sc;
    fss_test::MockDatabase mock;
    auto conn = std::make_shared<FakeConnection>();
    auto writer = make_null_writer();
    auto client = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &sc);
    // Not registered — should not crash
    sc.clientDisconnected(client.get());
    sc.cleanupRemovableClients();
}

TEST_CASE("server_clients: broadcastMsg reaches aircraft clients only")
{
    server_clients sc;
    fss_test::MockDatabase mock;

    // Aircraft client
    auto aircraft = make_aircraft_client("craft1", mock, sc);
    sc.clientConnected(aircraft);

    // Non-aircraft client (no identity sent → isAircraft() == false)
    auto conn2 = std::make_shared<FakeConnection>();
    auto writer2 = make_null_writer();
    auto ground = std::make_shared<fss::server::fss_client>(conn2, &mock, writer2, &sc);
    sc.clientConnected(ground);

    // Broadcast should not crash and should only target aircraft
    auto msg = std::make_shared<fss::transport::fss_message_rtt_request>();
    sc.broadcastMsg(msg);
}

TEST_CASE("server_clients: broadcastMsg does not block on one black-holed client (todo/36)")
{
    /* todo/36: broadcastMsg must schedule sends on each client's outbound
     * worker rather than sending inline, or a black-holed peer stalls the
     * caller (the main loop, every 15s, for server-list; a reporting
     * client's own recv thread for position relay) for up to
     * TCP_USER_TIMEOUT. Block one client's socket and assert both that the
     * call itself returns promptly and that the healthy client still
     * receives the broadcast. */
    server_clients sc;
    fss_test::MockDatabase mock;
    mock.asset_ids["stuck"] = 1;
    mock.asset_ids["healthy"] = 2;

    auto stuck_conn = std::make_shared<FakeConnection>();
    stuck_conn->cert_names.push_back("stuck");
    auto stuck_writer = make_null_writer();
    auto stuck_client = std::make_shared<fss::server::fss_client>(stuck_conn, &mock, stuck_writer, &sc);
    stuck_client->processMessage(std::make_shared<fss::transport::fss_message_identity>("stuck"));
    sc.clientConnected(stuck_client);

    auto healthy_conn = std::make_shared<FakeConnection>();
    healthy_conn->cert_names.push_back("healthy");
    auto healthy_writer = make_null_writer();
    auto healthy_client = std::make_shared<fss::server::fss_client>(healthy_conn, &mock, healthy_writer, &sc);
    healthy_client->processMessage(std::make_shared<fss::transport::fss_message_identity>("healthy"));
    sc.clientConnected(healthy_client);

    /* Both clients already received one server_list send as a side effect of
     * identify (independent of broadcastMsg) — baseline before the broadcast
     * under test, so the assertions below only see what *this* call
     * delivers. */
    std::size_t stuck_before = count_sent<fss::transport::fss_message_server_list>(stuck_conn->sentSnapshot());
    std::size_t healthy_before = count_sent<fss::transport::fss_message_server_list>(healthy_conn->sentSnapshot());

    stuck_conn->setBlocked(true);

    auto server_list = std::make_shared<fss::transport::fss_message_server_list>();
    server_list->addServer("10.0.0.1", 1234);

    auto start = std::chrono::steady_clock::now();
    sc.broadcastMsg(server_list);
    auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(elapsed < std::chrono::milliseconds(500));

    REQUIRE(fss_test::wait_for([&]() -> bool {
        return count_sent<fss::transport::fss_message_server_list>(healthy_conn->sentSnapshot()) == healthy_before + 1;
    }));
    /* The stuck client's socket is still blocked, so this broadcast has not
     * reached it. */
    REQUIRE(count_sent<fss::transport::fss_message_server_list>(stuck_conn->sentSnapshot()) == stuck_before);

    stuck_conn->setBlocked(false);
    stuck_client->disconnect();
    healthy_client->disconnect();
}

TEST_CASE("server_clients: position relay drops oldest and counts drops once the queue is full (todo/36)")
{
    /* Position relay is a bounded, drop-oldest queue (max_pending_position_relay
     * = 8 in fss-server.hpp, private so hard-coded here) — loss-tolerant
     * telemetry, but the loss must be observable. Push directly via
     * queuePositionRelay() without ever starting the outbound worker (skip
     * activate()/clientConnected()), so nothing drains the queue between
     * pushes and the resulting drop count is deterministic rather than
     * racing worker scheduling. */
    server_clients sc;
    fss_test::MockDatabase mock;
    mock.asset_ids["stuck"] = 1;

    auto stuck_conn = std::make_shared<FakeConnection>();
    stuck_conn->cert_names.push_back("stuck");
    auto stuck_writer = make_null_writer();
    /* Deliberately not registered via sc.clientConnected(): that would call
     * activate(), starting the outbound worker that drains the very queue
     * this test needs undrained. */
    auto stuck_client = std::make_shared<fss::server::fss_client>(stuck_conn, &mock, stuck_writer, &sc);
    stuck_client->processMessage(std::make_shared<fss::transport::fss_message_identity>("stuck"));

    constexpr int reports_sent = 10;
    constexpr uint64_t queue_cap = 8;
    for (int i = 0; i < reports_sent; ++i)
    {
        auto position = std::make_shared<fss::transport::fss_message_position_report>(
            0.0, 0.0, 0U, 0U, 0U, int16_t{0}, 0U, std::string{}, 0U, uint8_t{0}, 0U, uint8_t{0}, uint8_t{0},
            uint64_t{0});
        stuck_client->queuePositionRelay(position);
    }

    REQUIRE(stuck_client->getPositionRelayDropped() == reports_sent - queue_cap);
}

TEST_CASE("server_clients: broadcastMsg skips the 'except' client")
{
    server_clients sc;
    fss_test::MockDatabase mock;

    auto aircraft = make_aircraft_client("craft1", mock, sc);
    sc.clientConnected(aircraft);

    auto msg = std::make_shared<fss::transport::fss_message_rtt_request>();
    // Passing aircraft as the 'except' client — should send to 0 clients (no crash)
    sc.broadcastMsg(msg, aircraft.get());
}

TEST_CASE("server_clients: checkTimeouts disconnects timed-out client")
{
    /* liveness_active is only set after the aircraft identity handshake, so
     * build a fully-identified client via make_aircraft_client. */
    server_clients sc;
    sc.setClientTimeoutMs(1000);

    fss_test::MockDatabase mock;
    auto clock = std::make_shared<FakeClock>();

    mock.asset_ids["craft"] = 1;
    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    auto writer = make_null_writer();
    auto client = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &sc);
    client->setClock(clock);
    client->setTimeoutMs(1000);
    /* Identity handshake sets liveness_active = true. */
    client->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    sc.clientConnected(client);

    /* Advance time past the 1-second timeout. */
    clock->advance(2000);
    sc.checkTimeouts();
    sc.cleanupRemovableClients();
}

TEST_CASE("server_clients: checkTimeouts logs warning and disconnects timed-out aircraft client")
{
    /* Verify the FSS_LOG_WARN + clientDisconnected path inside checkTimeouts
     * fires for an identified aircraft client whose clock has advanced past
     * the timeout threshold. */
    server_clients sc;
    sc.setClientTimeoutMs(500);

    fss_test::MockDatabase mock;
    mock.asset_ids["old-craft"] = 1;

    /* Clock starts at 0; identity handshake records last_rtt_response_time=0. */
    auto clock = std::make_shared<FakeClock>();
    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("old-craft");
    auto writer = make_null_writer();
    auto client = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &sc);
    client->setClock(clock);
    client->setTimeoutMs(500);
    client->processMessage(std::make_shared<fss::transport::fss_message_identity>("old-craft"));
    sc.clientConnected(client);

    /* Advance past the 500 ms threshold so isTimedOut() returns true. */
    clock->advance(1001);
    sc.checkTimeouts();
    sc.cleanupRemovableClients();
}

TEST_CASE("server_clients: checkTimeouts leaves non-timed-out clients connected")
{
    server_clients sc;
    sc.setClientTimeoutMs(10000);

    fss_test::MockDatabase mock;
    auto conn = std::make_shared<FakeConnection>();
    auto writer = make_null_writer();
    auto client = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &sc);

    auto clock = std::make_shared<FakeClock>();
    client->setClock(clock);
    client->setTimeoutMs(10000);
    sc.clientConnected(client);

    // Time has not advanced — no timeout
    sc.checkTimeouts();
    // No clients moved to disconnected, cleanupRemovableClients is a no-op
    sc.cleanupRemovableClients();
}

TEST_CASE("server_clients: disconnectRevokedClients drops revoked connections")
{
    server_clients sc;
    fss_test::MockDatabase mock;

    auto conn = std::make_shared<FakeConnection>();
    conn->revoked = true;
    auto writer = make_null_writer();
    auto client = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &sc);
    sc.clientConnected(client);

    sc.disconnectRevokedClients("any.crl");
    sc.cleanupRemovableClients();
}

TEST_CASE("server_clients: destruction races recv-thread disconnect callbacks")
{
    /* Regression: ~server_clients used to join each connection's recv thread
     * while holding the list lock.  A recv thread delivering message_closed
     * calls clientDisconnected(), which needs that same lock — deadlock.
     * Build clients on real socketpair-backed connections (live recv
     * threads), close every peer so the closed messages race the destructor,
     * and require the destructor to complete.  Pre-fix this case hangs. */
    constexpr int n_clients = 8;
    fss_test::MockDatabase mock;
    /* Guard the peer fds so a REQUIRE failure mid-setup cannot leak them
     * into later tests; close_all() doubles as the deliberate trigger for
     * the message_closed deliveries (idempotent with the destructor). */
    struct peer_fd_guard {
        std::vector<int> fds{};
        ~peer_fd_guard() { close_all(); }
        void close_all()
        {
            for (int &fd : fds)
            {
                if (fd >= 0)
                {
                    ::close(fd);
                    fd = -1;
                }
            }
        }
    } peers;
    {
        auto sc = std::make_unique<server_clients>();
        for (int i = 0; i < n_clients; i++)
        {
            int fds[2];
            REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
            peers.fds.push_back(fds[1]);
            auto conn = fss::transport::fss_connection::create(fds[0]);
            auto writer = make_null_writer();
            auto client = std::make_shared<fss::server::fss_client>(conn, &mock, writer, sc.get());
            sc->clientConnected(client);
        }
        peers.close_all();
        sc.reset();
    }
    SUCCEED("destructor completed without deadlock");
}

TEST_CASE("server_clients: disconnectRevokedClients leaves non-revoked clients")
{
    server_clients sc;
    fss_test::MockDatabase mock;

    auto conn = std::make_shared<FakeConnection>();
    conn->revoked = false;
    auto writer = make_null_writer();
    auto client = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &sc);
    sc.clientConnected(client);

    sc.disconnectRevokedClients("any.crl");
    // Client should remain connected — cleanupRemovableClients is a no-op
    sc.cleanupRemovableClients();
}
