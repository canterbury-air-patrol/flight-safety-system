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

/* SKIP the test if TEST_DB_HOST is unset; otherwise build a db_connection from
 * the TEST_DB_* env vars and REQUIRE it has connected.  Returns a connection
 * the caller can use directly — no further SKIP/isConnected boilerplate. */
auto live_db_or_skip() -> std::unique_ptr<flight_safety_system::server::db_connection>
{
    const char *host = std::getenv("TEST_DB_HOST");
    if (host == nullptr)
    {
        SKIP("TEST_DB_HOST not set");
    }
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

TEST_CASE("db_connection: connects to live database")
{
    /* live_db_or_skip() asserts isConnected before returning. */
    (void)live_db_or_skip();
}

TEST_CASE("db_connection: getAssetId returns non-zero for pre-inserted asset")
{
    auto dbc = live_db_or_skip();
    REQUIRE(dbc->getAssetId("test-asset") != 0);
}

TEST_CASE("db_connection: recordRtt writes a row without error")
{
    auto dbc = live_db_or_skip();
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    dbc->recordRtt(asset_id, uint64_t{42});
}

TEST_CASE("db_connection: recordStatus writes a row without error")
{
    auto dbc = live_db_or_skip();
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    dbc->recordStatus(asset_id, uint8_t{80}, uint32_t{1000}, 12.4);
}

TEST_CASE("db_connection: recordSearchStatus writes a row without error")
{
    auto dbc = live_db_or_skip();
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    dbc->recordSearchStatus(asset_id, uint64_t{1}, uint64_t{50}, uint64_t{100});
}

TEST_CASE("db_connection: recordPosition with valid altitude inserts row")
{
    auto dbc = live_db_or_skip();
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    dbc->recordPosition(asset_id, -43.5, 172.6, uint32_t{100});
}

TEST_CASE("db_connection: recordPosition with overflow altitude is discarded")
{
    auto dbc = live_db_or_skip();
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    constexpr auto huge_alt = static_cast<uint32_t>(std::numeric_limits<int>::max()) + uint32_t{1};
    dbc->recordPosition(asset_id, -43.5, 172.6, huge_alt);
}

TEST_CASE("db_connection: getSmmSettings returns non-null for configured asset")
{
    auto dbc = live_db_or_skip();
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
    auto dbc = live_db_or_skip();
    REQUIRE(dbc->getSmmSettings(uint64_t{999999}) == nullptr);
}

TEST_CASE("db_connection: getActiveServers returns pre-configured server")
{
    auto dbc = live_db_or_skip();
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

TEST_CASE("db_connection: tryReconnectIfNeeded returns when connection is healthy")
{
    auto dbc = live_db_or_skip();
    dbc->tryReconnectIfNeeded();
    REQUIRE(dbc->isConnected());
}

TEST_CASE("db_connection: tryReconnectIfNeeded reconnects after underlying disconnect")
{
    auto dbc = live_db_or_skip();
    /* Force the underlying ECPG connection closed so db_ping() will fail,
     * which triggers the reconnect branch in tryReconnectIfNeeded(). */
    db_disconnect();
    dbc->tryReconnectIfNeeded();
    REQUIRE(dbc->isConnected());
}

TEST_CASE("db_connection: getCommand returns non-null for asset with pending command")
{
    /* The test fixture inserts an RTL command for test-asset before the test
     * suite runs.  getCommand() must find it and return a non-null result. */
    auto dbc = live_db_or_skip();
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    auto cmd = dbc->getCommand(asset_id);
    REQUIRE(cmd != nullptr);
}
