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
#include <optional>
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

TEST_CASE("db_connection: getCommands short-circuits empty input without a round-trip")
{
    /* An empty asset set returns before any DB access (it would also build an
     * invalid "IN ()" clause), so it must succeed even against a connection
     * that never came up -- the poller relies on this when no client is yet
     * identified. */
    flight_safety_system::server::db_connection dbc(
        "db.invalid", 5432, "user", flight_safety_system::secure_string(std::string_view{"pass"}), "db");
    auto commands = dbc.getCommands({});
    REQUIRE(commands.has_value()); /* engaged and empty, not a failed read */
    REQUIRE(commands->empty());
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

/* getAssetId returns nullopt on a DB read failure, 0 for a genuinely unknown
 * asset (todo/60). Most tests just want the "test-asset" fixture's id and
 * should hard-fail if either variant fires, so unwrap both here rather than
 * repeating the two checks at every call site. */
auto get_test_asset_id(flight_safety_system::server::db_connection &dbc) -> uint64_t
{
    auto id = dbc.getAssetId("test-asset");
    REQUIRE(id.has_value());
    REQUIRE(*id != 0);
    return *id;
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
    get_test_asset_id(*dbc);
}

TEST_CASE("db_connection: getAssetId returns nullopt when the read connection is down (todo/60)")
{
    /* Distinguishes a DB read failure from a genuinely unknown asset: before
     * todo/60, both collapsed to 0 and the identify path could not tell a
     * bad CN from a read outage. Force the read connection down (no
     * reconnect) so the query is guaranteed to fail at the ECPG layer, the
     * same technique the todo/34 write-side test uses. */
    LIVE_DB_OR_SKIP(dbc);
    db_disconnect(flight_safety_system::server::db_connection::read_conn_name);
    REQUIRE_FALSE(dbc->getAssetId("test-asset").has_value());
}

TEST_CASE("db_connection: getCommand returns nullopt when the read connection is down (todo/69)")
{
    /* test-asset has a pending command, so an engaged result here would be a
     * non-null one. With the read connection forced down the read cannot say
     * anything: nullopt, never the engaged nullptr that means "nothing
     * pending" — the caller must not clear a pending command over this. */
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = get_test_asset_id(*dbc);
    db_disconnect(flight_safety_system::server::db_connection::read_conn_name);
    REQUIRE_FALSE(dbc->getCommand(asset_id).has_value());
}

TEST_CASE("db_connection: getSmmSettings returns nullopt when the read connection is down (todo/69)")
{
    /* test-asset has SMM settings configured, so this is the case that used to
     * wipe a good cache: the failure arrived as the same nullptr an asset with
     * no settings row produces. */
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = get_test_asset_id(*dbc);
    db_disconnect(flight_safety_system::server::db_connection::read_conn_name);
    REQUIRE_FALSE(dbc->getSmmSettings(asset_id).has_value());
}

TEST_CASE("db_connection: getCommands returns nullopt when the read connection is down (todo/69)")
{
    /* The batched poll path. A partial map would be indistinguishable from
     * "these assets have no pending command", so the read reports failure and
     * the partial is discarded. */
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = get_test_asset_id(*dbc);
    db_disconnect(flight_safety_system::server::db_connection::read_conn_name);
    REQUIRE_FALSE(dbc->getCommands({asset_id}).has_value());
}

TEST_CASE("db_connection: recordRtt writes a row without error")
{
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = get_test_asset_id(*dbc);
    dbc->recordRtt(asset_id, uint64_t{42});
}

TEST_CASE("db_connection: recordStatus writes a row without error")
{
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = get_test_asset_id(*dbc);
    dbc->recordStatus(asset_id, uint8_t{80}, uint32_t{1000}, 12.4);
}

TEST_CASE("db_connection: recordSearchStatus writes a row without error")
{
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = get_test_asset_id(*dbc);
    dbc->recordSearchStatus(asset_id, uint64_t{1}, uint64_t{50}, uint64_t{100});
}

TEST_CASE("db_connection: recordPosition with valid altitude inserts row")
{
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = get_test_asset_id(*dbc);
    dbc->recordPosition(asset_id, -43.5, 172.6, uint32_t{100});
}

TEST_CASE("db_connection: recordPosition with overflow altitude is discarded")
{
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = get_test_asset_id(*dbc);
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
    auto asset_id = get_test_asset_id(*dbc);
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
    REQUIRE(threw_database_error([&] { dbc->recordCommandAck(uint64_t{1}, uint8_t{1}, uint64_t{1}, uint8_t{0}); }));
}

TEST_CASE("db_connection: getSmmSettings returns non-null for configured asset")
{
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = get_test_asset_id(*dbc);
    auto settings = dbc->getSmmSettings(asset_id);
    REQUIRE(settings.has_value());
    REQUIRE(*settings != nullptr);
    REQUIRE_FALSE((*settings)->getAddress().empty());
    REQUIRE((*settings)->getUsername() == "testuser");
    REQUIRE_FALSE((*settings)->getPassword().empty());
}

TEST_CASE("db_connection: getSmmSettings returns an engaged null for asset with no config")
{
    /* A successful read finding no settings row: engaged, holding nullptr —
     * distinct from the nullopt a failed read returns (todo/69). */
    LIVE_DB_OR_SKIP(dbc);
    auto settings = dbc->getSmmSettings(uint64_t{999999});
    REQUIRE(settings.has_value());
    REQUIRE(*settings == nullptr);
}

TEST_CASE("db_connection: getActiveServers returns pre-configured server")
{
    LIVE_DB_OR_SKIP(dbc);
    auto servers = dbc->getActiveServers();
    REQUIRE(servers.has_value());
    REQUIRE_FALSE(servers->empty());
    bool found = false;
    for (auto &s : *servers)
    {
        if (s.getAddress() == "fss.example.com" && s.getPort() == 20202)
        {
            found = true;
        }
    }
    REQUIRE(found);
}

namespace {
/* Run one statement against the live test database via psql, mirroring
 * docker/db-unit-test-entrypoint.sh's own fixture-seeding approach: no raw SQL
 * capability is exposed through db_connection's public API, and some fixtures
 * (and every DDL change) cannot be expressed through it. Returns psql's exit
 * status. `sql` is built from literals in this file, never from anything a
 * client could reach. */
auto run_psql(const std::string &sql) -> int
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
    cmd += " -v ON_ERROR_STOP=1 -c \"";
    cmd += sql;
    cmd += "\" > /dev/null";
    return std::system(cmd.c_str());
}

/* Flip the todo/41 truncation-fixture row's active flag. The row is seeded
 * inactive by that script so no other getActiveServers() test ever sees it;
 * this helper activates it only for the duration of one test. */
auto set_truncated_server_active(bool active) -> int
{
    std::string sql = "UPDATE config_serverconfig SET active = ";
    sql += (active ? "true" : "false");
    sql += " WHERE name = 'test-server-truncated'";
    return run_psql(sql);
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

TEST_CASE("db_connection: getActiveServers fails the read when a server address is truncated (todo/41)")
{
    /* A FETCH that truncates server_address ends the cursor loop with a
     * warning (sqlcode >= 0), not an error. Before todo/41's fix, getActive
     * Servers() would see fetch_error == 0 and return whatever was
     * accumulated before the bad row as the complete set -- silently
     * dropping every server sorted after it. It must fail the read instead
     * (nullopt, exactly like a mid-cursor read error; todo/24 turned the old
     * database_error throw into this status return) so the poller keeps the
     * previous good cache rather than shipping a partial list to aircraft. */
    LIVE_DB_OR_SKIP(dbc);
    scoped_truncated_server_active guard;
    REQUIRE_FALSE(dbc->getActiveServers().has_value());
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
    auto asset_id = get_test_asset_id(*dbc);

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
    auto asset_id = get_test_asset_id(*dbc);
    auto cmd = dbc->getCommand(asset_id);
    REQUIRE(cmd.has_value());
    REQUIRE(*cmd != nullptr);
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

TEST_CASE("db_connection: getCommands returns the newest command per asset and omits absent ones")
{
    /* Exercises the batched read end-to-end: DISTINCT ON must pick the newest
     * command for the asset, the IN-list must filter to only the requested
     * asset, and an id with no command must be absent (not a null entry). */
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = get_test_asset_id(*dbc);
    insert_test_command(asset_id);
    auto newest_id = insert_test_command(asset_id);

    constexpr uint64_t absent_asset = 999999999ULL; /* no such asset -> no command */
    auto commands = dbc->getCommands({asset_id, absent_asset});

    REQUIRE(commands.has_value());
    REQUIRE(commands->count(asset_id) == 1);
    REQUIRE(commands->at(asset_id)->getDBId() == newest_id);
    REQUIRE(commands->count(absent_asset) == 0);
}

TEST_CASE("db_connection: getCommands agrees with getCommand for the same asset")
{
    /* The batched and single-row reads must never disagree about which row is
     * newest, or the poller would dispatch a different command than the
     * identify-time path. */
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = get_test_asset_id(*dbc);
    insert_test_command(asset_id);

    auto single = dbc->getCommand(asset_id);
    auto batch = dbc->getCommands({asset_id});
    REQUIRE(single.has_value());
    REQUIRE(*single != nullptr);
    REQUIRE(batch.has_value());
    REQUIRE(batch->count(asset_id) == 1);
    REQUIRE(batch->at(asset_id)->getDBId() == (*single)->getDBId());
}

TEST_CASE("db_connection: recordCommandDispatch stores the dispatch id")
{
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = get_test_asset_id(*dbc);
    auto command_id = insert_test_command(asset_id);

    dbc->recordCommandDispatch(command_id, uint64_t{4242});

    REQUIRE(get_command_column(command_id, "dispatch_id") == "4242");
}

TEST_CASE("db_connection: recordCommandAck stores ack_state, ack_timestamp and ack_superseded_by")
{
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = get_test_asset_id(*dbc);
    auto command_id = insert_test_command(asset_id);
    dbc->recordCommandDispatch(command_id, uint64_t{5555});

    dbc->recordCommandAck(command_id, static_cast<uint8_t>(flight_safety_system::transport::command_ack_superseded),
                          uint64_t{1700000000000},
                          static_cast<uint8_t>(flight_safety_system::transport::supersede_low_battery));

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
    auto asset_id = get_test_asset_id(*dbc);
    auto command_id = insert_test_command(asset_id);
    dbc->recordCommandDispatch(command_id, uint64_t{6666});

    dbc->recordCommandAck(command_id, static_cast<uint8_t>(flight_safety_system::transport::command_ack_actioned),
                          uint64_t{100}, static_cast<uint8_t>(flight_safety_system::transport::supersede_none));
    dbc->recordCommandAck(command_id, static_cast<uint8_t>(flight_safety_system::transport::command_ack_received),
                          uint64_t{200}, static_cast<uint8_t>(flight_safety_system::transport::supersede_none));

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
    auto asset_id = get_test_asset_id(*dbc);

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

        dbc->recordCommandAck(command_id, static_cast<uint8_t>(flight_safety_system::transport::command_ack_actioned),
                              uint64_t{100}, static_cast<uint8_t>(flight_safety_system::transport::supersede_none));
        dbc->recordCommandAck(command_id, later_state, uint64_t{200},
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
    auto asset_id = get_test_asset_id(*dbc);
    auto command_id = insert_test_command(asset_id);
    dbc->recordCommandDispatch(command_id, uint64_t{7780});

    dbc->recordCommandAck(command_id, static_cast<uint8_t>(flight_safety_system::transport::command_ack_received),
                          uint64_t{100}, static_cast<uint8_t>(flight_safety_system::transport::supersede_none));
    REQUIRE(get_command_column(command_id, "ack_state") == "0");

    dbc->recordCommandAck(command_id, static_cast<uint8_t>(flight_safety_system::transport::command_ack_actioned),
                          uint64_t{200}, static_cast<uint8_t>(flight_safety_system::transport::supersede_none));
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
    auto asset_id = get_test_asset_id(*dbc);
    auto command_id = insert_test_command(asset_id);
    dbc->recordCommandDispatch(command_id, uint64_t{7790});
    dbc->recordCommandAck(command_id, static_cast<uint8_t>(flight_safety_system::transport::command_ack_actioned),
                          uint64_t{100}, static_cast<uint8_t>(flight_safety_system::transport::supersede_none));
    REQUIRE(get_command_column(command_id, "ack_state") == "1");

    /* Redispatch (e.g. after a reconnect, with the new connection's id). */
    dbc->recordCommandDispatch(command_id, uint64_t{7791});
    REQUIRE(get_command_column(command_id, "ack_state").empty());
    REQUIRE(get_command_column(command_id, "ack_timestamp").empty());
    REQUIRE(get_command_column(command_id, "ack_superseded_by").empty());

    /* The re-ack for the new delivery lands, terminal over the reopened row. */
    dbc->recordCommandAck(command_id, static_cast<uint8_t>(flight_safety_system::transport::command_ack_superseded),
                          uint64_t{200}, static_cast<uint8_t>(flight_safety_system::transport::supersede_comms_loss));
    REQUIRE(get_command_column(command_id, "ack_state") == "2");
    REQUIRE(get_command_column(command_id, "ack_timestamp") == "200");
    REQUIRE(get_command_column(command_id, "ack_superseded_by") == "2");
}

TEST_CASE("db_connection: recordCommandAck updates only the row it names")
{
    /* todo/68: the ack is keyed on the command row's primary key, which the
     * acking session resolved from the id it dispatched. Two commands for the
     * same asset -- the shape that used to force an (asset_id, dispatch_id)
     * match plus a newest-row subselect, and the shape a colliding dispatch id
     * could land on the wrong one of -- must now be independent. Ack the OLDER
     * row and confirm the newer one is untouched: under the old newest-row
     * reconstruction this was not expressible at all. */
    LIVE_DB_OR_SKIP(dbc);
    auto asset_id = get_test_asset_id(*dbc);
    auto older_command = insert_test_command(asset_id);
    auto newer_command = insert_test_command(asset_id);
    REQUIRE(older_command != newer_command);

    /* The same dispatch id on both, which is realistic: it restarts at 0 on
     * every connection, so two deliveries across two sessions collide. */
    dbc->recordCommandDispatch(older_command, uint64_t{8800});
    dbc->recordCommandDispatch(newer_command, uint64_t{8800});

    dbc->recordCommandAck(older_command, static_cast<uint8_t>(flight_safety_system::transport::command_ack_actioned),
                          uint64_t{100}, static_cast<uint8_t>(flight_safety_system::transport::supersede_none));

    REQUIRE(get_command_column(older_command, "ack_state") == "1");
    REQUIRE(get_command_column(older_command, "ack_timestamp") == "100");
    REQUIRE(get_command_column(newer_command, "ack_state").empty());
    REQUIRE(get_command_column(newer_command, "ack_timestamp").empty());
}

TEST_CASE("db_connection: verifySchema accepts the provisioned schema")
{
    /* The negative case below is only meaningful if the positive one holds:
     * required_columns[] in server-db.pgc must not have drifted ahead of the
     * schema the suite actually runs against (e2e/schema/001_init.sql, mounted
     * by docker-compose.db-unit-tests.yaml). A failure here means the list and
     * the schema disagree -- which is the whole point of the check, fired at
     * the one moment it is cheap to fix. */
    LIVE_DB_OR_SKIP(dbc);
    REQUIRE(dbc->verifySchema());
}

namespace {
/* RAII: drops one column for the duration of a test and puts it back. The
 * restore is unconditional -- the unit suite shares one long-lived database
 * across every TEST_CASE, so a column left dropped would fail every later test
 * that touches assets_assetcommand rather than just this one.
 *
 * CHECK, not REQUIRE, in the destructor: it is implicitly noexcept, and a
 * REQUIRE failure throws Catch2's test-failure exception, which would call
 * std::terminate when thrown while already unwinding from the test body's own
 * failure (same reasoning as scoped_truncated_server_active above). */
class scoped_dropped_column {
private:
    std::string table_;
    std::string column_;
    std::string type_;
public:
    scoped_dropped_column(std::string table, std::string column, std::string type)
        : table_(std::move(table)), column_(std::move(column)), type_(std::move(type))
    {
        REQUIRE(run_psql("ALTER TABLE " + table_ + " DROP COLUMN " + column_) == 0);
    }
    scoped_dropped_column(const scoped_dropped_column &) = delete;
    scoped_dropped_column(scoped_dropped_column &&) = delete;
    auto operator=(const scoped_dropped_column &) -> scoped_dropped_column & = delete;
    auto operator=(scoped_dropped_column &&) -> scoped_dropped_column & = delete;
    ~scoped_dropped_column() { CHECK(run_psql("ALTER TABLE " + table_ + " ADD COLUMN " + column_ + " " + type_) == 0); }
};
} // namespace

TEST_CASE("db_connection: verifySchema refuses a database missing an ack column (todo/73)")
{
    /* The failure this exists to catch: fss-web deployed without the migration
     * that adds the command-ack columns. Every db_command_set_dispatch_id and
     * db_command_record_ack would then fail, the write queue would count those
     * failures, and the fail-safe would sever the whole fleet ~5 s after the
     * first command -- with no way back, because the schema is still wrong.
     * Refusing to start is the whole remedy, so assert it at the seam that
     * decides. The process-level half (exit status, and the log naming the
     * column) is e2e/test_schema_check.py.
     *
     * ack_state is nullable with no default, index or constraint, so dropping
     * and re-adding it restores the table exactly. */
    LIVE_DB_OR_SKIP(dbc);
    scoped_dropped_column guard("assets_assetcommand", "ack_state", "SMALLINT");
    REQUIRE_FALSE(dbc->verifySchema());
}
