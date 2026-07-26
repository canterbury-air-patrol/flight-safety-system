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

#include "fss-transport.hpp"
#include "fss-server.hpp"
#include "fss-client-ssl.hpp"
#include "mock_database.hpp"
#include "db-write-queue.hpp"
#include "test_helpers.hpp"

namespace fss = flight_safety_system;

namespace {

/* ── shared test doubles ─────────────────────────────────── */

struct FakeClock : public fss::IClock {
    uint64_t t{0};
    auto now_ms() const -> uint64_t override { return t; }
    void advance(uint64_t ms) { t += ms; }
};

class FakeConnection : public fss::transport::fss_connection {
public:
    std::list<std::string> cert_names{};
    FakeConnection() = default;
    FakeConnection(const FakeConnection &) = delete;
    FakeConnection(FakeConnection &&) = delete;
    auto operator=(const FakeConnection &) -> FakeConnection & = delete;
    auto operator=(FakeConnection &&) -> FakeConnection & = delete;
    ~FakeConnection() override = default;
    auto getClientNames() -> std::list<std::string> override { return cert_names; }
protected:
    auto sendMsg(const std::shared_ptr<fss::transport::buf_len> &) -> bool override { return true; }
};

/* These tests never send message types that invoke the writer (rtt_response,
 * position_report, system_status, search_status), so a no-op sink suffices. */
auto make_null_writer() -> std::shared_ptr<fss::server::db_write_queue>
{
    return std::make_shared<fss::server::db_write_queue>(std::size_t{64},
                                                         [](const fss::server::db_write_task &) -> void {});
}

class NullClientHandler : public fss::server::fss_client_handler {
public:
    int disconnects{0};
    ~NullClientHandler() override = default;
    void clientDisconnected(fss::server::fss_client *) override { ++disconnects; }
    void broadcastMsg(const std::shared_ptr<fss::transport::fss_message> &,
                      fss::server::fss_client * = nullptr) override
    {
    }
};

/* ── server-side (fss_client) timeout tests ─────────────── */

} // namespace

TEST_CASE("timeout: isTimedOut false for unidentified client within identify deadline")
{
    fss_test::MockDatabase mock;
    auto conn = std::make_shared<FakeConnection>();
    NullClientHandler handler;
    auto writer = make_null_writer();
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    auto clock = std::make_shared<FakeClock>();
    session->setClock(clock);
    session->setIdentifyTimeoutMs(30000);
    session->activate();

    clock->advance(29999);
    REQUIRE_FALSE(session->isTimedOut());
}

TEST_CASE("timeout: isTimedOut true for unidentified client past identify deadline")
{
    fss_test::MockDatabase mock;
    auto conn = std::make_shared<FakeConnection>();
    NullClientHandler handler;
    auto writer = make_null_writer();
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    auto clock = std::make_shared<FakeClock>();
    session->setClock(clock);
    session->setIdentifyTimeoutMs(30000);
    session->activate();

    clock->advance(30001);
    REQUIRE(session->isTimedOut());
}

TEST_CASE("timeout: identified client not pruned by identify deadline")
{
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;
    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto writer = make_null_writer();
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    auto clock = std::make_shared<FakeClock>();
    session->setClock(clock);
    /* Use a very short identify deadline so advancing past it is easy. */
    session->setIdentifyTimeoutMs(5000);
    session->activate();

    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    /* Advance past the identify deadline but stay within the liveness window.
     * Identification resets last_rtt_response_time, so the liveness clock
     * starts here; the session must NOT be pruned by the identify deadline. */
    clock->advance(10000);
    REQUIRE_FALSE(session->isTimedOut());
}

TEST_CASE("timeout: isTimedOut false right after identification")
{
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;
    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto writer = make_null_writer();
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    auto clock = std::make_shared<FakeClock>();
    session->setClock(clock);

    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    REQUIRE_FALSE(session->isTimedOut());
}

TEST_CASE("timeout: isTimedOut true after timeout without RTT response")
{
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;
    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto writer = make_null_writer();
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    auto clock = std::make_shared<FakeClock>();
    session->setClock(clock);

    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    clock->advance(30001);
    REQUIRE(session->isTimedOut());
}

TEST_CASE("timeout: isTimedOut reset by RTT response")
{
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;
    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto writer = make_null_writer();
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    auto clock = std::make_shared<FakeClock>();
    session->setClock(clock);

    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    /* Send an RTT request so the session tracks a pending request id.
     * fss_connection::sendMsg overwrites the message id — capture it after. */
    auto rtt_req = std::make_shared<fss::transport::fss_message_rtt_request>();
    session->sendRTTRequest(rtt_req);
    uint64_t assigned_id = rtt_req->getId();

    clock->advance(20000);

    /* RTT response arrives at t=20000, resetting the liveness clock. */
    session->processMessage(std::make_shared<fss::transport::fss_message_rtt_response>(assigned_id));

    /* 25 s after the response, still within the 30 s window. */
    clock->advance(25000);
    REQUIRE_FALSE(session->isTimedOut());

    /* Another 6 s takes us past the window. */
    clock->advance(6000);
    REQUIRE(session->isTimedOut());
}

TEST_CASE("timeout: setTimeoutMs overrides default threshold")
{
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;
    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto writer = make_null_writer();
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    auto clock = std::make_shared<FakeClock>();
    session->setClock(clock);
    session->setTimeoutMs(5000);

    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    clock->advance(5000);
    REQUIRE_FALSE(session->isTimedOut());

    clock->advance(1);
    REQUIRE(session->isTimedOut());
}

/* ── client-side (fss_server) timeout tests ─────────────── */

namespace {

/* A server that pretends to be connected so it can be placed in
 * fss_client's connected-servers list without a real TLS handshake. */
class AlwaysConnectedServer : public fss::client_ssl::fss_server {
public:
    using fss_server::fss_server;
    AlwaysConnectedServer(const AlwaysConnectedServer &) = delete;
    AlwaysConnectedServer(AlwaysConnectedServer &&) = delete;
    auto operator=(const AlwaysConnectedServer &) -> AlwaysConnectedServer & = delete;
    auto operator=(AlwaysConnectedServer &&) -> AlwaysConnectedServer & = delete;
    ~AlwaysConnectedServer() override = default;
    auto connected() -> bool override { return true; }
protected:
    auto reconnect_to() -> bool override { return false; }
};

/* A server whose reconnect() succeeds by installing a fresh in-memory
 * connection, for exercising post-reconnect state. */
class ReconnectingServer : public fss::client_ssl::fss_server {
public:
    using fss_server::fss_server;
    ReconnectingServer(const ReconnectingServer &) = delete;
    ReconnectingServer(ReconnectingServer &&) = delete;
    auto operator=(const ReconnectingServer &) -> ReconnectingServer & = delete;
    auto operator=(ReconnectingServer &&) -> ReconnectingServer & = delete;
    ~ReconnectingServer() override = default;
    auto connected() -> bool override { return true; }
    /* Incremented on the outbound worker thread and read by the test thread
     * since todo/66 moved the dial off the caller's thread. */
    std::atomic<int> reconnects{0};
protected:
    auto reconnect_to() -> bool override
    {
        ++reconnects;
        this->setConnection(std::make_shared<FakeConnection>());
        return true;
    }
};

/* A client that exposes addServer and captures status changes. */
class TestClient : public fss::client_ssl::fss_client {
public:
    using fss_client::fss_client;
    int status_changes{0};
    fss::client_ssl::connection_status last_status{fss::client_ssl::CLIENT_CONNECTION_STATUS_UNKNOWN};

    void add(const std::shared_ptr<fss::client_ssl::fss_server> &server) { this->addServer(server); }
protected:
    void connectionStatusChange(fss::client_ssl::connection_status status) override
    {
        ++status_changes;
        last_status = status;
    }
};

} // namespace

TEST_CASE("timeout: isServerTimedOut false before any message")
{
    auto client = std::make_shared<TestClient>();
    auto server = std::make_shared<AlwaysConnectedServer>(client.get(), "127.0.0.1", uint16_t{9999}, "", "", "");
    auto clock = std::make_shared<FakeClock>();
    server->setClock(clock);

    clock->advance(100000);
    REQUIRE_FALSE(server->isServerTimedOut());
}

TEST_CASE("timeout: isServerTimedOut false right after message")
{
    auto client = std::make_shared<TestClient>();
    auto server = std::make_shared<AlwaysConnectedServer>(client.get(), "127.0.0.1", uint16_t{9999}, "", "", "");
    auto clock = std::make_shared<FakeClock>();
    server->setClock(clock);

    /* An RTT request from the server starts the liveness clock. */
    server->processMessage(std::make_shared<fss::transport::fss_message_rtt_request>());

    clock->advance(29999);
    REQUIRE_FALSE(server->isServerTimedOut());
}

TEST_CASE("timeout: isServerTimedOut true after timeout without messages")
{
    auto client = std::make_shared<TestClient>();
    auto server = std::make_shared<AlwaysConnectedServer>(client.get(), "127.0.0.1", uint16_t{9999}, "", "", "");
    auto clock = std::make_shared<FakeClock>();
    server->setClock(clock);

    server->processMessage(std::make_shared<fss::transport::fss_message_rtt_request>());

    clock->advance(30001);
    REQUIRE(server->isServerTimedOut());
}

TEST_CASE("timeout: attemptReconnect moves timed-out server to reconnect queue")
{
    auto client = std::make_shared<TestClient>();
    auto server = std::make_shared<AlwaysConnectedServer>(client.get(), "127.0.0.1", uint16_t{9999}, "", "", "");
    auto clock = std::make_shared<FakeClock>();
    server->setClock(clock);

    /* Register the server as "connected" with the client. */
    client->add(server);

    /* Start liveness, then let it expire. */
    server->processMessage(std::make_shared<fss::transport::fss_message_rtt_request>());
    clock->advance(30001);

    REQUIRE(server->isServerTimedOut());

    client->attemptReconnect();

    /* connectionStatusChange should have fired with DISCONNECTED since the
     * server moved from servers to reconnect_servers. */
    REQUIRE(client->status_changes > 0);
    REQUIRE(client->last_status == fss::client_ssl::CLIENT_CONNECTION_STATUS_DISCONNECTED);
}

TEST_CASE("timeout: successful reconnect restores cold-start liveness")
{
    /* todo/65: reconnect() used to leave liveness armed against the previous
     * connection's last-received timestamp, so a reconnected server whose
     * peer had nothing to say yet was flagged timed out again on the very
     * next attemptReconnect() tick — tearing down a healthy connection every
     * reconnect interval, forever. A fresh connection must restart from the
     * cold-connect state: liveness disarmed until the first message arrives
     * on THAT connection. */
    auto client = std::make_shared<TestClient>();
    auto server = std::make_shared<ReconnectingServer>(client.get(), "127.0.0.1", uint16_t{9999}, "", "", "");
    auto clock = std::make_shared<FakeClock>();
    server->setClock(clock);
    client->add(server);

    /* Arm liveness, then let it expire; the reconnect driver tears down and
     * redials in the same attemptReconnect() pass. */
    server->processMessage(std::make_shared<fss::transport::fss_message_rtt_request>());
    clock->advance(30001);
    REQUIRE(server->isServerTimedOut());

    /* attemptReconnect() schedules the dial on the server's outbound worker
     * rather than blocking on it (todo/66), so the redial lands asynchronously
     * and the following pass is the one that harvests it into the live list. */
    client->attemptReconnect();
    REQUIRE(fss_test::wait_for([&]() -> bool { return server->reconnects.load() == 1; }));
    client->attemptReconnect();
    REQUIRE(client->last_status == fss::client_ssl::CLIENT_CONNECTION_STATUS_CONNECTED_1_SERVER);

    /* Nothing has been received on the new connection: however long it stays
     * quiet, it must not be timed out on the old connection's stale clock. */
    clock->advance(30001);
    REQUIRE_FALSE(server->isServerTimedOut());
    clock->advance(1000000);
    REQUIRE_FALSE(server->isServerTimedOut());

    /* The first message on the new connection re-arms liveness normally. */
    server->processMessage(std::make_shared<fss::transport::fss_message_rtt_request>());
    clock->advance(29999);
    REQUIRE_FALSE(server->isServerTimedOut());
    clock->advance(2);
    REQUIRE(server->isServerTimedOut());
}
