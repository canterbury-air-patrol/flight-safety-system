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

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "fss-server.hpp"

extern "C" {
#include "server-db.h"
}

TEST_CASE("db_connection: invalid host reports not connected")
{
    /* Use a host that is guaranteed to refuse the connection so the test
     * does not depend on a running PostgreSQL instance. */
    flight_safety_system::server::db_connection dbc(
        "db.invalid", 5432, "user", flight_safety_system::secure_string(std::string_view{"pass"}), "db");
    REQUIRE_FALSE(dbc.isConnected());
}

TEST_CASE("db_connection: construction bounds in-flight TCP stalls via PGTCPUSERTIMEOUT")
{
    /* db_connect()'s run-once env block must set PGTCPUSERTIMEOUT so a
     * black-holed connection with a query in flight fails on the keepalive
     * clock instead of the kernel's retransmission timeout (todo/46). Any
     * construction latches the env block, connected or not. Only presence is
     * asserted: overwrite=0 means an operator override must win, so the
     * value may legitimately differ from the built-in default. */
    flight_safety_system::server::db_connection dbc(
        "db.invalid", 5432, "user", flight_safety_system::secure_string(std::string_view{"pass"}), "db");
    REQUIRE(std::getenv("PGTCPUSERTIMEOUT") != nullptr);
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
        host, port, user != nullptr ? user : "postgres",
        flight_safety_system::secure_string(std::string_view{pass != nullptr ? pass : "password"}),
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

TEST_CASE("db_connection: write methods throw database_error when the write connection is down (todo/34)")
{
    /* Before todo/34's fix, db_position_create_entry et al. (server-db.pgc)
     * checked nothing after EXEC SQL -- a failed INSERT/UPDATE just printed
     * via sqlprint() and the C++ wrapper returned normally, so db_write_queue
     * never saw an exception and write_failure_count() never moved. Force
     * the write connection down (no reconnect) so the write is guaranteed to
     * fail, and require every write method surfaces it as database_error. */
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    db_disconnect(flight_safety_system::server::db_connection::write_conn_name);

    auto threw_database_error = [](auto &&write_call) -> bool {
        try
        {
            write_call();
        }
        catch (const flight_safety_system::server::database_error &)
        {
            return true;
        }
        return false;
    };

    REQUIRE(threw_database_error([&] { dbc->recordPosition(asset_id, -43.5, 172.6, uint32_t{100}); }));
    REQUIRE(threw_database_error([&] { dbc->recordRtt(asset_id, uint64_t{42}); }));
    REQUIRE(threw_database_error([&] { dbc->recordStatus(asset_id, uint8_t{80}, uint32_t{1000}, 12.4); }));
    REQUIRE(threw_database_error([&] { dbc->recordSearchStatus(asset_id, uint64_t{1}, uint64_t{50}, uint64_t{100}); }));
    REQUIRE(threw_database_error([&] { dbc->recordCommandDispatch(uint64_t{1}, uint64_t{1}); }));
    REQUIRE(threw_database_error(
        [&] { dbc->recordCommandAck(asset_id, uint64_t{1}, uint8_t{1}, uint64_t{1}, uint8_t{0}); }));
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

namespace {

/* Run a psql query against the TEST_DB_* database and return its unaligned,
 * tuples-only output with the trailing newline stripped -- e.g. a single
 * column's value, or an empty string for SQL NULL. recordCommandDispatch and
 * recordCommandAck have no read-back accessor of their own (asset_command
 * only exposes the command fields, not the ack columns), so this is the only
 * way to verify what they actually wrote. */
auto psql_query(const std::string &sql) -> std::string
{
    const char *host = std::getenv("TEST_DB_HOST");
    const char *port = std::getenv("TEST_DB_PORT");
    const char *user = std::getenv("TEST_DB_USER");
    const char *pass = std::getenv("TEST_DB_PASS");
    const char *dbname = std::getenv("TEST_DB_NAME");
    std::string cmd = "PGPASSWORD='";
    cmd += (pass != nullptr ? pass : "password");
    cmd += "' psql -t -A -h ";
    cmd += (host != nullptr ? host : "db");
    cmd += " -p ";
    cmd += (port != nullptr ? port : "5432");
    cmd += " -U ";
    cmd += (user != nullptr ? user : "postgres");
    cmd += " -d ";
    cmd += (dbname != nullptr ? dbname : "postgres");
    cmd += " -c \"";
    cmd += sql;
    cmd += "\"";

    std::array<char, 256> buffer{};
    std::string result;
    FILE *pipe = popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
    {
        result += buffer.data();
    }
    REQUIRE(pclose(pipe) == 0);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
    {
        result.pop_back();
    }
    return result;
}

/* Inserts a fresh command row for asset_id and returns its id, so each test
 * targets its own row rather than mutating the shared test-asset RTL fixture
 * (which other tests, e.g. getCommand above, read without expecting its
 * dispatch/ack columns to change). */
auto insert_test_command(uint64_t asset_id) -> uint64_t
{
    std::string sql = "INSERT INTO assets_assetcommand (asset_id, command) VALUES (";
    sql += std::to_string(asset_id);
    sql += ", 'RTL') RETURNING id";
    return std::stoull(psql_query(sql));
}

auto get_command_column(uint64_t command_id, const std::string &column) -> std::string
{
    std::string sql = "SELECT ";
    sql += column;
    sql += " FROM assets_assetcommand WHERE id = ";
    sql += std::to_string(command_id);
    return psql_query(sql);
}

} // namespace

TEST_CASE("db_connection: recordCommandDispatch stores the dispatch id")
{
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    auto command_id = insert_test_command(asset_id);

    dbc->recordCommandDispatch(command_id, uint64_t{4242});

    REQUIRE(get_command_column(command_id, "dispatch_id") == "4242");
}

TEST_CASE("db_connection: recordCommandAck stores ack_state, ack_timestamp and ack_superseded_by")
{
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    auto command_id = insert_test_command(asset_id);
    dbc->recordCommandDispatch(command_id, uint64_t{5555});

    dbc->recordCommandAck(
        asset_id, uint64_t{5555}, static_cast<uint8_t>(flight_safety_system::transport::command_ack_superseded),
        uint64_t{1700000000000}, static_cast<uint8_t>(flight_safety_system::transport::supersede_low_battery));

    REQUIRE(get_command_column(command_id, "ack_state") == "2");
    REQUIRE(get_command_column(command_id, "ack_timestamp") == "1700000000000");
    REQUIRE(get_command_column(command_id, "ack_superseded_by") == "1");
}

TEST_CASE("db_connection: recordCommandAck does not regress an already-terminal ack")
{
    /* A late "received" (state 0) must never clobber a settled outcome -- see
     * the comment above the UPDATE in db_command_record_ack(). Simulate an
     * out-of-order ack arrival: actioned first, then a stale "received". */
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    auto command_id = insert_test_command(asset_id);
    dbc->recordCommandDispatch(command_id, uint64_t{6666});

    dbc->recordCommandAck(asset_id, uint64_t{6666},
                          static_cast<uint8_t>(flight_safety_system::transport::command_ack_actioned), uint64_t{100},
                          static_cast<uint8_t>(flight_safety_system::transport::supersede_none));
    dbc->recordCommandAck(asset_id, uint64_t{6666},
                          static_cast<uint8_t>(flight_safety_system::transport::command_ack_received), uint64_t{200},
                          static_cast<uint8_t>(flight_safety_system::transport::supersede_none));

    REQUIRE(get_command_column(command_id, "ack_state") == "1");
    REQUIRE(get_command_column(command_id, "ack_timestamp") == "100");
}

TEST_CASE("db_connection: recordCommandAck refuses a second terminal outcome")
{
    /* todo/48: a terminal outcome is final for its dispatch. A later terminal
     * ack (superseded/rejected/noop) from a buggy, misordered or forged peer
     * must not rewrite a settled "actioned" in the audit record -- the
     * query-enforced writable set is stored-state NULL or received only. */
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);

    const std::array<uint8_t, 3> later_terminals = {
        static_cast<uint8_t>(flight_safety_system::transport::command_ack_superseded),
        static_cast<uint8_t>(flight_safety_system::transport::command_ack_rejected),
        static_cast<uint8_t>(flight_safety_system::transport::command_ack_noop),
    };
    uint64_t dispatch_id = 7770;
    for (const auto later_state : later_terminals)
    {
        auto command_id = insert_test_command(asset_id);
        dbc->recordCommandDispatch(command_id, dispatch_id);

        dbc->recordCommandAck(asset_id, dispatch_id,
                              static_cast<uint8_t>(flight_safety_system::transport::command_ack_actioned),
                              uint64_t{100}, static_cast<uint8_t>(flight_safety_system::transport::supersede_none));
        dbc->recordCommandAck(asset_id, dispatch_id, later_state, uint64_t{200},
                              static_cast<uint8_t>(flight_safety_system::transport::supersede_low_battery));

        REQUIRE(get_command_column(command_id, "ack_state") == "1");
        REQUIRE(get_command_column(command_id, "ack_timestamp") == "100");
        REQUIRE(get_command_column(command_id, "ack_superseded_by") == "0");
        dispatch_id++;
    }
}

TEST_CASE("db_connection: recordCommandAck admits the conforming received-then-terminal flow")
{
    /* The two-phase ack a conforming FMU sends (received, then exactly one
     * terminal outcome) must be unaffected by the todo/48 finality guard. */
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    auto command_id = insert_test_command(asset_id);
    dbc->recordCommandDispatch(command_id, uint64_t{7780});

    dbc->recordCommandAck(asset_id, uint64_t{7780},
                          static_cast<uint8_t>(flight_safety_system::transport::command_ack_received), uint64_t{100},
                          static_cast<uint8_t>(flight_safety_system::transport::supersede_none));
    REQUIRE(get_command_column(command_id, "ack_state") == "0");

    dbc->recordCommandAck(asset_id, uint64_t{7780},
                          static_cast<uint8_t>(flight_safety_system::transport::command_ack_actioned), uint64_t{200},
                          static_cast<uint8_t>(flight_safety_system::transport::supersede_none));
    REQUIRE(get_command_column(command_id, "ack_state") == "1");
    REQUIRE(get_command_column(command_id, "ack_timestamp") == "200");
}

TEST_CASE("db_connection: recordCommandDispatch reopens the ack cycle")
{
    /* todo/48's finality is scoped to the latest dispatch: redelivery (identify
     * resends the newest command on every reconnect) records a new dispatch,
     * which clears the ack columns so the redelivered command's re-ack is
     * admitted rather than refused as a duplicate terminal. Pinned end-to-end
     * by test_server_restart.py's post-bounce re-ack check; this covers the
     * same contract at the query level. */
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = dbc->getAssetId("test-asset");
    REQUIRE(asset_id != 0);
    auto command_id = insert_test_command(asset_id);
    dbc->recordCommandDispatch(command_id, uint64_t{7790});
    dbc->recordCommandAck(asset_id, uint64_t{7790},
                          static_cast<uint8_t>(flight_safety_system::transport::command_ack_actioned), uint64_t{100},
                          static_cast<uint8_t>(flight_safety_system::transport::supersede_none));
    REQUIRE(get_command_column(command_id, "ack_state") == "1");

    /* Redispatch (e.g. after a reconnect, with the new connection's id). */
    dbc->recordCommandDispatch(command_id, uint64_t{7791});
    REQUIRE(get_command_column(command_id, "ack_state").empty());
    REQUIRE(get_command_column(command_id, "ack_timestamp").empty());
    REQUIRE(get_command_column(command_id, "ack_superseded_by").empty());

    /* The re-ack for the new delivery lands, terminal over the reopened row. */
    dbc->recordCommandAck(asset_id, uint64_t{7791},
                          static_cast<uint8_t>(flight_safety_system::transport::command_ack_superseded), uint64_t{200},
                          static_cast<uint8_t>(flight_safety_system::transport::supersede_comms_loss));
    REQUIRE(get_command_column(command_id, "ack_state") == "2");
    REQUIRE(get_command_column(command_id, "ack_timestamp") == "200");
    REQUIRE(get_command_column(command_id, "ack_superseded_by") == "2");
}
