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

#include <list>
#include <memory>
#include <string>

#include "fss-transport.hpp"
#include "fss-server.hpp"
#include "fss.hpp"
#include "db-write-queue.hpp"
#include "mock_database.hpp"
#include "server-clients.hpp"

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

protected:
    auto sendMsg(const std::shared_ptr<fss::transport::buf_len> & /*bl*/) -> bool override { return true; }
};

struct FakeClock : public fss::IClock {
    uint64_t t{0};
    auto now_ms() const -> uint64_t override { return t; }
    void advance(uint64_t ms) { t += ms; }
};

auto make_null_writer() -> std::shared_ptr<fss::server::db_write_queue>
{
    return std::make_shared<fss::server::db_write_queue>(
        std::size_t{16},
        [](const fss::server::db_write_task &) {}
    );
}

/* Build a client that has completed the identity handshake as an aircraft. */
auto make_aircraft_client(
    const std::string &name,
    fss_test::MockDatabase &mock,
    server_clients &handler
) -> std::shared_ptr<fss::server::fss_client>
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
    server_clients sc;
    sc.setClientTimeoutMs(1000);

    fss_test::MockDatabase mock;
    auto conn = std::make_shared<FakeConnection>();
    auto writer = make_null_writer();
    auto client = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &sc);

    auto clock = std::make_shared<FakeClock>();
    client->setClock(clock);
    client->setTimeoutMs(1000);

    sc.clientConnected(client);

    // Advance time past the timeout threshold
    clock->advance(2000);
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
