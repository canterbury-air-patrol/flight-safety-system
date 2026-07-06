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

#include <cstdlib>
#include <limits>
#include <memory>
#include <string>

#include "fss-server.hpp"

extern "C" {
#include "server-db.h"
}

TEST_CASE("db_connection: invalid host reports not connected")
{
    /* Use a host that is guaranteed to refuse the connection so the test
     * does not depend on a running PostgreSQL instance. */
    flight_safety_system::server::db_connection dbc("db.invalid", 5432, "user", "pass", "db");
    REQUIRE_FALSE(dbc.isConnected());
}

namespace {

/* Build a db_connection from the TEST_DB_* env vars and REQUIRE it has
 * connected.  Precondition: TEST_DB_HOST is set — call sites guard that with
 * LIVE_DB_OR_SKIP below, which skips the test when no database is configured. */
auto make_live_db() -> std::unique_ptr<flight_safety_system::server::db_connection>
{
    const char *host = std::getenv("TEST_DB_HOST");
    int port = 5432;
    if (const char *p = std::getenv("TEST_DB_PORT"))
    {
        port = std::stoi(p);
    }
    const char *user = std::getenv("TEST_DB_USER");
    const char *pass = std::getenv("TEST_DB_PASS");
    const char *dbname = std::getenv("TEST_DB_NAME");
    auto dbc = std::make_unique<flight_safety_system::server::db_connection>(
        host, port, user != nullptr ? user : "postgres", pass != nullptr ? pass : "password",
        dbname != nullptr ? dbname : "postgres");
    REQUIRE(dbc->isConnected());
    return dbc;
}

} // namespace

/* Catch2's SKIP() macro arrived in 3.3; distro packages can be older (Debian
 * bookworm ships Catch2 2.x).  Where SKIP is missing, fall back to marking the
 * test passed and bailing out of the current TEST_CASE. */
#ifndef SKIP
#define SKIP(msg)                                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        SUCCEED(msg);                                                                                                  \
        return;                                                                                                        \
    } while (0)
#endif

/* Skip the current TEST_CASE when no live database is configured; otherwise
 * bind `name` to a freshly connected db_connection.  `name` is a declarator,
 * so it cannot be parenthesised — hence the bugprone-macro-parentheses waiver. */
#define LIVE_DB_OR_SKIP(name)                                                                                          \
    if (std::getenv("TEST_DB_HOST") == nullptr)                                                                        \
    {                                                                                                                  \
        SKIP("TEST_DB_HOST not set");                                                                                  \
    }                                                                                                                  \
    auto name = make_live_db() // NOLINT(bugprone-macro-parentheses)

TEST_CASE("db_connection: connects to live database")
{
    /* make_live_db() asserts isConnected before returning. */
    LIVE_DB_OR_SKIP(dbc);
    (void)dbc;
}

TEST_CASE("db_connection: getAssetId returns non-zero for pre-inserted asset")
{
    LIVE_DB_OR_SKIP(dbc);
    REQUIRE(dbc->getAssetId("test-asset") != 0);
}

TEST_CASE("db_connection: recordRtt writes a row without error")
{
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    dbc->recordRtt(asset_id, uint64_t{42});
}

TEST_CASE("db_connection: recordStatus writes a row without error")
{
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    dbc->recordStatus(asset_id, uint8_t{80}, uint32_t{1000}, 12.4);
}

TEST_CASE("db_connection: recordSearchStatus writes a row without error")
{
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    dbc->recordSearchStatus(asset_id, uint64_t{1}, uint64_t{50}, uint64_t{100});
}

TEST_CASE("db_connection: recordPosition with valid altitude inserts row")
{
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    dbc->recordPosition(asset_id, -43.5, 172.6, uint32_t{100});
}

TEST_CASE("db_connection: recordPosition with overflow altitude is discarded")
{
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    constexpr auto huge_alt = static_cast<uint32_t>(std::numeric_limits<int>::max()) + uint32_t{1};
    dbc->recordPosition(asset_id, -43.5, 172.6, huge_alt);
}

TEST_CASE("db_connection: getSmmSettings returns non-null for configured asset")
{
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    auto settings = dbc->getSmmSettings(asset_id);
    REQUIRE(settings != nullptr);
    REQUIRE_FALSE(settings->getAddress().empty());
    REQUIRE(settings->getUsername() == "testuser");
    REQUIRE_FALSE(settings->getPassword().empty());
}

TEST_CASE("db_connection: getSmmSettings returns null for asset with no config")
{
    LIVE_DB_OR_SKIP(dbc);
    REQUIRE(dbc->getSmmSettings(uint64_t{999999}) == nullptr);
}

TEST_CASE("db_connection: getActiveServers returns pre-configured server")
{
    LIVE_DB_OR_SKIP(dbc);
    auto servers = dbc->getActiveServers();
    REQUIRE_FALSE(servers.empty());
    bool found = false;
    for (auto &s : servers)
    {
        if (s.getAddress() == "fss.example.com" && s.getPort() == 20202)
        {
            found = true;
        }
    }
    REQUIRE(found);
}

namespace {
/* Flip the todo/41 truncation-fixture row's active flag via psql, mirroring
 * docker/db-unit-test-entrypoint.sh's own fixture-seeding approach (no raw
 * SQL capability is exposed through db_connection's public API). The row is
 * seeded inactive by that script so no other getActiveServers() test ever
 * sees it; this helper activates it only for the duration of one test. */
auto set_truncated_server_active(bool active) -> int
{
    const char *host = std::getenv("TEST_DB_HOST");
    const char *port = std::getenv("TEST_DB_PORT");
    const char *user = std::getenv("TEST_DB_USER");
    const char *pass = std::getenv("TEST_DB_PASS");
    const char *dbname = std::getenv("TEST_DB_NAME");
    std::string cmd = "PGPASSWORD='";
    cmd += (pass != nullptr ? pass : "password");
    cmd += "' psql -h ";
    cmd += (host != nullptr ? host : "db");
    cmd += " -p ";
    cmd += (port != nullptr ? port : "5432");
    cmd += " -U ";
    cmd += (user != nullptr ? user : "postgres");
    cmd += " -d ";
    cmd += (dbname != nullptr ? dbname : "postgres");
    cmd += " -c \"UPDATE config_serverconfig SET active = ";
    cmd += (active ? "true" : "false");
    cmd += " WHERE name = 'test-server-truncated'\" > /dev/null";
    return std::system(cmd.c_str());
}

/* RAII: guarantees the fixture row is deactivated again even if the body of
 * the test that activated it fails an assertion partway through. The
 * deactivate call in the destructor uses CHECK, not REQUIRE: this destructor
 * is implicitly noexcept, and a REQUIRE failure throws Catch2's internal
 * test-failure exception, which would call std::terminate if thrown from
 * here -- especially likely to happen exactly when unwinding from the
 * activating test's own REQUIRE failure. */
class scoped_truncated_server_active {
public:
    scoped_truncated_server_active() { REQUIRE(set_truncated_server_active(true) == 0); }
    scoped_truncated_server_active(const scoped_truncated_server_active &) = delete;
    scoped_truncated_server_active(scoped_truncated_server_active &&) = delete;
    auto operator=(const scoped_truncated_server_active &) -> scoped_truncated_server_active & = delete;
    auto operator=(scoped_truncated_server_active &&) -> scoped_truncated_server_active & = delete;
    ~scoped_truncated_server_active() { CHECK(set_truncated_server_active(false) == 0); }
};
} // namespace

TEST_CASE("db_connection: getActiveServers throws when a server address is truncated (todo/41)")
{
    /* A FETCH that truncates server_address ends the cursor loop with a
     * warning (sqlcode >= 0), not an error. Before todo/41's fix, getActive
     * Servers() would see fetch_error == 0 and return whatever was
     * accumulated before the bad row as the complete set -- silently
     * dropping every server sorted after it. It must now throw
     * database_error instead, exactly like a mid-cursor read error, so the
     * poller's exception_guard keeps the previous good cache rather than
     * shipping a partial list to aircraft. */
    LIVE_DB_OR_SKIP(dbc);
    scoped_truncated_server_active guard;
    /* Assert the throw manually rather than via REQUIRE_THROWS_AS: older Catch2
     * expands that macro to a by-value catch clause, which trips
     * -Werror=catch-value on the polymorphic database_error type. */
    bool threw_database_error = false;
    try
    {
        dbc->getActiveServers();
    }
    catch (const flight_safety_system::server::database_error &)
    {
        threw_database_error = true;
    }
    REQUIRE(threw_database_error);
}

TEST_CASE("db_connection: tryReconnectIfNeeded returns when connection is healthy")
{
    LIVE_DB_OR_SKIP(dbc);
    dbc->tryReconnectIfNeeded();
    REQUIRE(dbc->isConnected());
}

TEST_CASE("db_connection: tryReconnectIfNeeded reconnects after underlying disconnect")
{
    LIVE_DB_OR_SKIP(dbc);
    /* Force both underlying ECPG connections closed so db_ping() fails on
     * each, triggering the reconnect branch in tryReconnectIfNeeded(). */
    db_disconnect(flight_safety_system::server::db_connection::read_conn_name);
    db_disconnect(flight_safety_system::server::db_connection::write_conn_name);
    dbc->tryReconnectIfNeeded();
    REQUIRE(dbc->isConnected());
}

TEST_CASE("db_connection: tryReconnectIfNeeded restores a single dropped connection")
{
    /* The read and write paths use independent ECPG connections. If only one
     * is lost, tryReconnectIfNeeded must restore it — and operations on that
     * connection must work again — while the other is left untouched. */
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);

    /* Drop one connection by name, recover it, then exercise both a read
     * (getAssetId) and a write (recordRtt): whichever connection was dropped
     * must be restored, and the untouched one must keep working. */
    auto recovers_after_dropping = [&](const char *conn_name) {
        db_disconnect(conn_name);
        dbc->tryReconnectIfNeeded();
        REQUIRE(dbc->isConnected());
        REQUIRE(dbc->getAssetId("test-asset") == asset_id);
        dbc->recordRtt(asset_id, uint64_t{7});
        REQUIRE(dbc->isConnected());
    };

    SECTION("only the read connection drops")
    {
        recovers_after_dropping(flight_safety_system::server::db_connection::read_conn_name);
    }

    SECTION("only the write connection drops")
    {
        recovers_after_dropping(flight_safety_system::server::db_connection::write_conn_name);
    }
}

TEST_CASE("db_connection: getCommand returns non-null for asset with pending command")
{
    /* The test fixture inserts an RTL command for test-asset before the test
     * suite runs.  getCommand() must find it and return a non-null result. */
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    auto cmd = dbc->getCommand(asset_id);
    REQUIRE(cmd != nullptr);
}
