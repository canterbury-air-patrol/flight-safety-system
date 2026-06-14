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

#include <cmath>
#include <limits>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "fss-transport.hpp"
#include "fss-server.hpp"
#include "mock_database.hpp"
#include "db-write-queue.hpp"
#include "test_helpers.hpp"

namespace fss = flight_safety_system;

namespace {

/* Stand-in for the real fss_connection used by fss_client. The default
 * fss_connection ctor leaves fd = -1 and does NOT spawn a recv thread,
 * so we can drive processMessage from the test synchronously.
 *
 * Interception point: sendMsg(fss_message) internally calls the virtual
 * sendMsg(buf_len). We override that one and decode the framed buffer
 * back into an fss_message so tests can assert on outbound traffic. */
class FakeConnection : public fss::transport::fss_connection {
public:
    std::vector<std::shared_ptr<fss::transport::fss_message>> sent{};
    std::list<std::string> cert_names{};

    FakeConnection() = default;
    FakeConnection(const FakeConnection &) = delete;
    FakeConnection(FakeConnection &&) = delete;
    auto operator=(const FakeConnection &) -> FakeConnection & = delete;
    auto operator=(FakeConnection &&) -> FakeConnection & = delete;
    ~FakeConnection() override = default;

    auto getClientNames() -> std::list<std::string> override { return cert_names; }
protected:
    auto sendMsg(const std::shared_ptr<fss::transport::buf_len> &bl) -> bool override
    {
        auto msg = fss::transport::fss_message::decode(bl);
        if (msg != nullptr)
        {
            sent.push_back(msg);
        }
        return true;
    }
};

/* Return the first message in `sent` (at or after `from`) that decoded to a T,
 * or nullptr if no such message is present. */
template<typename T>
auto find_sent(const std::vector<std::shared_ptr<fss::transport::fss_message>> &sent, std::size_t from = 0)
    -> std::shared_ptr<T>
{
    for (std::size_t i = from; i < sent.size(); ++i)
    {
        if (auto cast = std::dynamic_pointer_cast<T>(sent[i]))
        {
            return cast;
        }
    }
    return nullptr;
}

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

/* Builds a db_write_queue that forwards to the mock — tests that don't care
 * about telemetry still need a non-null writer for the fss_client ctor. */
auto make_mock_writer(fss_test::MockDatabase &mock) -> std::shared_ptr<fss::server::db_write_queue>
{
    auto sink = [&mock](const fss::server::db_write_task &task) -> void {
        std::visit(fss::server::overloaded{
                       [&](const fss::server::rtt_write &w) -> void { mock.recordRtt(w.asset_id, w.rtt_ms); },
                       [&](const fss::server::position_write &w) -> void {
                           mock.recordPosition(w.asset_id, w.latitude, w.longitude, w.altitude);
                       },
                       [&](const fss::server::status_write &w) -> void {
                           mock.recordStatus(w.asset_id, w.bat_percent, w.bat_mah_used, w.bat_voltage);
                       },
                       [&](const fss::server::search_status_write &w) -> void {
                           mock.recordSearchStatus(w.asset_id, w.search_id, w.completed, w.total);
                       },
                   },
                   task);
    };
    return std::make_shared<fss::server::db_write_queue>(std::size_t{1024}, sink);
}

class NullClientHandler : public fss::server::fss_client_handler {
public:
    int disconnects{0};
    std::vector<std::shared_ptr<fss::transport::fss_message>> broadcasts{};
    ~NullClientHandler() override = default;
    void clientDisconnected(fss::server::fss_client * /*client*/) override { ++disconnects; }
    void broadcastMsg(const std::shared_ptr<fss::transport::fss_message> &msg,
                      fss::server::fss_client * /*except*/ = nullptr) override
    {
        broadcasts.push_back(msg);
    }
};

} // namespace

TEST_CASE("session: rejects identify when claimed name does not match cert CN")
{
    /* Identity name must match a CN from the peer certificate.
     * A client presenting cert CN=craft must not be allowed to claim
     * identity "imposter" — that would let any holder of any valid
     * client cert masquerade as any aircraft. */
    fss_test::MockDatabase mock;
    mock.asset_ids["imposter"] = 1;
    mock.asset_ids["craft"] = 2;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("imposter"));

    REQUIRE(handler.disconnects > 0);
    /* No server-list / command messages should leak out before disconnect. */
    for (const auto &m : conn->sent)
    {
        REQUIRE(m->getType() != fss::transport::message_type_server_list);
        REQUIRE(m->getType() != fss::transport::message_type_command);
    }
}

TEST_CASE("session: rejects identify when no cert CN present")
{
    /* getClientNames() returns empty when the peer presented no cert (or
     * the leaf had no CN). The server must refuse to identify such a
     * connection rather than treating absence of a CN as permission to
     * claim any name. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    /* cert_names intentionally empty */
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    REQUIRE(handler.disconnects > 0);
}

TEST_CASE("session: rejects non-aircraft identify when no cert CN present")
{
    fss_test::MockDatabase mock;

    auto conn = std::make_shared<FakeConnection>();
    /* cert_names intentionally empty */
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity_non_aircraft>());

    REQUIRE(handler.disconnects > 0);
    REQUIRE_FALSE(session->isAircraft());
}

TEST_CASE("session: rejects non-aircraft identify when cert CN belongs to a known aircraft")
{
    fss_test::MockDatabase mock;
    mock.asset_ids["aircraft-1"] = 42;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("aircraft-1");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity_non_aircraft>());

    REQUIRE(handler.disconnects > 0);
    REQUIRE_FALSE(session->isAircraft());
}

TEST_CASE("session: accepts non-aircraft identify when cert CN is not a known aircraft")
{
    fss_test::MockDatabase mock;
    /* "ground-station-1" absent from asset_ids — not an aircraft */

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("ground-station-1");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    fss_test::capture_cerr cap;
    session->processMessage(std::make_shared<fss::transport::fss_message_identity_non_aircraft>());

    REQUIRE(handler.disconnects == 0);
    REQUIRE_FALSE(session->isAircraft());
    REQUIRE(cap.str().find("Non-aircraft client identified: ground-station-1") != std::string::npos);
}

TEST_CASE("session: identified non-aircraft client times out on liveness loss")
{
    /* A non-aircraft client must be reaped on RTT timeout just like an
     * aircraft; before liveness tracking was enabled for non-aircraft
     * identify, isTimedOut() always returned false for them. */
    fss_test::MockDatabase mock;
    auto clock = std::make_shared<FakeClock>();

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("ground-station-1");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->setClock(clock);
    session->setTimeoutMs(1000);

    /* Not timed out before identify (liveness inactive). */
    REQUIRE_FALSE(session->isTimedOut());

    session->processMessage(std::make_shared<fss::transport::fss_message_identity_non_aircraft>());
    REQUIRE_FALSE(session->isAircraft());

    /* Just under the threshold: still alive. */
    clock->advance(999);
    REQUIRE_FALSE(session->isTimedOut());

    /* Past the threshold with no RTT response: timed out. */
    clock->advance(2);
    REQUIRE(session->isTimedOut());
}

TEST_CASE("session: logs when an identified non-aircraft client disconnects")
{
    fss_test::MockDatabase mock;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("ground-station-1");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity_non_aircraft>());
    REQUIRE(handler.disconnects == 0);

    fss_test::capture_cerr cap;
    session->processMessage(std::make_shared<fss::transport::fss_message_closed>());

    REQUIRE(cap.str().find("Non-aircraft client disconnected: ground-station-1") != std::string::npos);
}

TEST_CASE("session: rejects identify when asset unknown to database")
{
    fss_test::MockDatabase mock;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("unknownAsset");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    auto identify = std::make_shared<fss::transport::fss_message_identity>("unknownAsset");
    session->processMessage(identify);

    REQUIRE(conn->sent.empty());
}

TEST_CASE("session: getCommand returns newest-timestamp entry")
{
    /* The production ECPG query is ORDER BY timestamp DESC LIMIT 1; the
     * mock replicates that. Injecting two commands, the session's
     * sendCommand must dispatch the one with the higher timestamp. */
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 42;
    mock.asset_ids["craft"] = asset_id;
    mock.pushCommand(asset_id, std::make_shared<fss::server::asset_command>(
                                   /*dbid*/ 1, /*ts*/ 100, "HOLD", 0.0, 0.0, 0));
    mock.pushCommand(asset_id, std::make_shared<fss::server::asset_command>(
                                   /*dbid*/ 2, /*ts*/ 500, "RTL", 0.0, 0.0, 0));

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    /* Drive the session to identify + send initial command. */
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    auto cmd = find_sent<fss::transport::fss_message_asset_command>(conn->sent);
    REQUIRE(cmd != nullptr);
    REQUIRE(cmd->getTimeStamp() == 500);
}

TEST_CASE("session: server list sent on identify contains seeded servers")
{
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;
    mock.active_servers.emplace_back("10.0.0.1", uint16_t{8080});
    mock.active_servers.emplace_back("10.0.0.2", uint16_t{9090});

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    auto server_list_msg = find_sent<fss::transport::fss_message_server_list>(conn->sent);
    REQUIRE(server_list_msg != nullptr);
    auto servers = server_list_msg->getServers();
    REQUIRE(servers.size() == 2);
    CHECK(servers[0].first == "10.0.0.1");
    CHECK(servers[0].second == 8080);
    CHECK(servers[1].first == "10.0.0.2");
    CHECK(servers[1].second == 9090);
}

TEST_CASE("session: rapid sendCommand does not duplicate a single pending command")
{
    /* sendCommand is called every 100ms to minimise command delivery
     * latency. That is only safe if repeated calls with the same pending
     * row do not resend the message. Verify idempotency by driving
     * sendCommand for 2s worth of ticks against one queued command and
     * counting command messages on the wire. */
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 7;
    mock.asset_ids["craft"] = asset_id;
    mock.pushCommand(asset_id, std::make_shared<fss::server::asset_command>(
                                   /*dbid*/ 1, /*ts*/ 100, "TERM", 0.0, 0.0, 0));

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    constexpr int ticks_per_sec = 1000 / fss::server::command_poll_ms;
    for (int i = 0; i < 2 * ticks_per_sec; ++i)
    {
        session->sendCommand();
    }

    REQUIRE(count_sent<fss::transport::fss_message_asset_command>(conn->sent) == 1);
}

TEST_CASE("session: a freshly queued command is delivered after the poller updates the cache")
{
    /* Commands are delivered in two steps: the background poller fetches
     * from the DB (simulated here by setPendingCommand) then the next
     * sendCommand tick transmits it. */
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 9;
    mock.asset_ids["craft"] = asset_id;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    const std::size_t sent_before = conn->sent.size();

    auto cmd = std::make_shared<fss::server::asset_command>(
        /*dbid*/ 42, /*ts*/ 500, "DISARM", 0.0, 0.0, 0);
    mock.pushCommand(asset_id, cmd);
    session->setPendingCommand(cmd); // simulate one poller tick
    session->sendCommand();

    bool delivered = false;
    for (std::size_t i = sent_before; i < conn->sent.size(); ++i)
    {
        if (conn->sent[i]->getType() == fss::transport::message_type_command)
        {
            delivered = true;
        }
    }
    REQUIRE(delivered);
}

TEST_CASE("session: RTT timeout disconnects client")
{
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto clock = std::make_shared<FakeClock>();
    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->setClock(clock);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    auto rtt_req1 = std::make_shared<fss::transport::fss_message_rtt_request>();
    session->sendRTTRequest(rtt_req1); // queued at t=0

    clock->advance(30001); // past rtt_timeout (30 s)

    auto rtt_req2 = std::make_shared<fss::transport::fss_message_rtt_request>();
    session->sendRTTRequest(rtt_req2); // triggers timeout → disconnect

    REQUIRE(handler.disconnects > 0);
}

TEST_CASE("session: version handshake stores negotiated version and replies")
{
    /* Version exchange: when the client opens with a version message,
     * the server records the negotiated version on the connection and
     * replies with its own version. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    REQUIRE(conn->getNegotiatedVersion() == fss::transport::FSS_PROTOCOL_VERSION_LEGACY);

    auto version = std::make_shared<fss::transport::fss_message_version>(fss::transport::FSS_PROTOCOL_VERSION,
                                                                         fss::transport::FSS_PROTOCOL_MIN_VERSION, 0U);
    session->processMessage(version);

    REQUIRE(conn->getNegotiatedVersion() == fss::transport::FSS_PROTOCOL_VERSION);
    REQUIRE(handler.disconnects == 0);

    bool saw_version_response = false;
    for (const auto &m : conn->sent)
    {
        if (m->getType() == fss::transport::message_type_version)
        {
            saw_version_response = true;
        }
    }
    REQUIRE(saw_version_response);
}

TEST_CASE("session: version handshake rejects incompatible peer")
{
    /* When the client's max version is below our minimum, the server
     * disconnects rather than continuing in an unsupported dialect. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    /* Peer claims to only support version 0 (max = 0). Our min is 1, so
     * this is a hard reject. */
    auto version = std::make_shared<fss::transport::fss_message_version>(
        /*version*/ 0U, /*min_version*/ 0U, /*flags*/ 0U);
    session->processMessage(version);

    REQUIRE(handler.disconnects > 0);
}

TEST_CASE("rate limiter: drops messages beyond burst capacity without disconnecting")
{
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto clock = std::make_shared<FakeClock>();
    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->setClock(clock);
    session->setRateLimits(100, 0); // 100-message burst, no refill

    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    auto pos = std::make_shared<fss::transport::fss_message_position_report>(
        0.0, 0.0, 0U, 0U, 0U, int16_t{0}, 0U, std::string{}, 0U, uint8_t{0}, 0U, uint8_t{0}, uint8_t{0}, uint64_t{0});
    for (int i = 0; i < 200; ++i)
    {
        session->processMessage(pos);
    }

    REQUIRE(handler.disconnects == 0);
    REQUIRE(handler.broadcasts.size() == 100);
}

TEST_CASE("rate limiter: refill allows messages after bucket drains")
{
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto clock = std::make_shared<FakeClock>();
    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->setClock(clock);
    session->setRateLimits(5, 10); // 5-message burst, 10/s refill

    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    auto pos = std::make_shared<fss::transport::fss_message_position_report>(
        0.0, 0.0, 0U, 0U, 0U, int16_t{0}, 0U, std::string{}, 0U, uint8_t{0}, 0U, uint8_t{0}, uint8_t{0}, uint64_t{0});
    for (int i = 0; i < 10; ++i)
    {
        session->processMessage(pos);
    }
    // 5 go through, 5 dropped
    REQUIRE(handler.broadcasts.size() == 5);

    // Advance 1 second → 10 new tokens (capped at 5)
    clock->advance(1000);
    for (int i = 0; i < 10; ++i)
    {
        session->processMessage(pos);
    }
    REQUIRE(handler.broadcasts.size() == 10);
    REQUIRE(handler.disconnects == 0);
}

TEST_CASE("session: legacy client (no version handshake) is accepted")
{
    /* Pre-versioning clients send identity directly. The server must
     * accept this and treat the connection as legacy (version 0). */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    /* No version message — go straight to identity, like an old client. */
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    REQUIRE(handler.disconnects == 0);
    REQUIRE(conn->getNegotiatedVersion() == fss::transport::FSS_PROTOCOL_VERSION_LEGACY);

    bool saw_server_list = false;
    for (const auto &m : conn->sent)
    {
        if (m->getType() == fss::transport::message_type_server_list)
        {
            saw_server_list = true;
        }
    }
    REQUIRE(saw_server_list);
}

/* Helper: build a minimal position report with a given timestamp. */
namespace {
auto make_position_msg(uint64_t ts) -> std::shared_ptr<fss::transport::fss_message_position_report>
{
    return std::make_shared<fss::transport::fss_message_position_report>(
        0.0, 0.0, 0U, 0U, 0U, int16_t{0}, 0U, std::string{}, 0U, uint8_t{0}, 0U, uint8_t{0}, uint8_t{0}, ts);
}

/* Drive a version + identity exchange over a v2 connection.
 * Returns the next seq the client should use for data messages. */
auto establish_v2_session(std::shared_ptr<fss::server::fss_client> &session, uint64_t &next_id) -> void
{
    auto version = std::make_shared<fss::transport::fss_message_version>(fss::transport::FSS_PROTOCOL_VERSION,
                                                                         fss::transport::FSS_PROTOCOL_MIN_VERSION, 0U);
    version->setId(next_id++);
    session->processMessage(version);

    auto identity = std::make_shared<fss::transport::fss_message_identity>("craft");
    identity->setId(next_id++);
    session->processMessage(identity);
}
} // namespace

TEST_CASE("session: v2 replayed data message is dropped")
{
    /* m7.1 phase 2: a position report whose seq matches a message the server
     * already processed (a duplicate within the session) must be discarded.
     * This is in-order/duplicate detection, not a security replay defence —
     * TLS already prevents record-layer replay. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    uint64_t next_id = 1;
    establish_v2_session(session, next_id);
    REQUIRE(handler.disconnects == 0);

    /* First position report — accepted. */
    auto pos = make_position_msg(fss::fss_current_timestamp());
    pos->setId(next_id);
    session->processMessage(pos);
    REQUIRE(handler.broadcasts.size() == 1);

    /* Replay the same message (same seq). */
    auto replay = make_position_msg(fss::fss_current_timestamp());
    replay->setId(next_id); // duplicate seq — replay
    session->processMessage(replay);
    REQUIRE(handler.broadcasts.size() == 1); // not forwarded
    REQUIRE(handler.disconnects == 0);       // connection stays up
}

TEST_CASE("session: duplicate version message is ignored without derailing the session")
{
    /* A buggy peer re-sending the version handshake mid-session must not
     * re-negotiate the protocol version (downgrade) nor re-anchor the
     * sequence check (which, for an out-of-order duplicate, would silently
     * drop all subsequent telemetry while the session looks alive). */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    uint64_t next_id = 1;
    establish_v2_session(session, next_id);

    auto pos = make_position_msg(fss::fss_current_timestamp());
    pos->setId(next_id++);
    session->processMessage(pos);
    REQUIRE(handler.broadcasts.size() == 1);

    /* In-sequence duplicate offering a lower version: must not downgrade. */
    auto dup = std::make_shared<fss::transport::fss_message_version>(1U, 1U, 0U);
    dup->setId(next_id++);
    session->processMessage(dup);
    REQUIRE(conn->getNegotiatedVersion() == fss::transport::FSS_PROTOCOL_VERSION);
    REQUIRE(handler.disconnects == 0);

    /* The duplicate consumed a sequence id; the stream must continue. */
    auto pos2 = make_position_msg(fss::fss_current_timestamp());
    pos2->setId(next_id++);
    session->processMessage(pos2);
    REQUIRE(handler.broadcasts.size() == 2);

    /* Out-of-sequence duplicate: ignored entirely, must not re-anchor the
     * sequence check to its id. */
    auto stray = std::make_shared<fss::transport::fss_message_version>(1U, 1U, 0U);
    stray->setId(99);
    session->processMessage(stray);

    auto pos3 = make_position_msg(fss::fss_current_timestamp());
    pos3->setId(next_id++);
    session->processMessage(pos3);
    REQUIRE(handler.broadcasts.size() == 3);
    REQUIRE(handler.disconnects == 0);
}

TEST_CASE("session: duplicate version warnings are throttled")
{
    /* The duplicate-version branch runs before the rate limiter, so a peer
     * spamming version messages must not flood the log: warn on the first
     * and every 100th only. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    uint64_t next_id = 1;
    establish_v2_session(session, next_id);

    fss_test::capture_cerr capture;
    fss_test::scoped_log_level level("warn");
    constexpr int duplicates = 250; /* warns at 1, 100, 200 → 3 lines */
    for (int i = 0; i < duplicates; i++)
    {
        auto dup = std::make_shared<fss::transport::fss_message_version>(1U, 1U, 0U);
        dup->setId(next_id++);
        session->processMessage(dup);
    }
    REQUIRE(handler.disconnects == 0);

    const std::string logged = capture.str();
    std::size_t occurrences = 0;
    for (std::size_t pos = logged.find("duplicate protocol version"); pos != std::string::npos;
         pos = logged.find("duplicate protocol version", pos + 1))
    {
        occurrences++;
    }
    REQUIRE(occurrences == 3);
}

TEST_CASE("session: v2 replayed identity disconnects")
{
    /* m7.1 phase 2: a replayed (or out-of-order) identity message must
     * cause a disconnect rather than silently failing validation. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    /* Send version(seq=1) → server sets expected_seq=2. */
    auto version = std::make_shared<fss::transport::fss_message_version>(fss::transport::FSS_PROTOCOL_VERSION,
                                                                         fss::transport::FSS_PROTOCOL_MIN_VERSION, 0U);
    version->setId(1);
    session->processMessage(version);
    REQUIRE(conn->getNegotiatedVersion() == fss::transport::FSS_PROTOCOL_VERSION);

    /* Send identity with wrong seq (e.g. 5 instead of 2). */
    auto identity = std::make_shared<fss::transport::fss_message_identity>("craft");
    identity->setId(5); // out-of-order
    session->processMessage(identity);

    REQUIRE(handler.disconnects > 0);
}

TEST_CASE("session: v1 client skips seq checking")
{
    /* m7.1 phase 2: seq checking only applies to negotiated version >= 2.
     * A v1 client must still be fully accepted regardless of its message IDs. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    auto version = std::make_shared<fss::transport::fss_message_version>(1U, 1U, 0U);
    version->setId(1);
    session->processMessage(version);
    REQUIRE(conn->getNegotiatedVersion() == 1U);

    /* Identify with an arbitrary, non-sequential id — must not trigger disconnect. */
    auto identity = std::make_shared<fss::transport::fss_message_identity>("craft");
    identity->setId(99);
    session->processMessage(identity);

    REQUIRE(handler.disconnects == 0);
}

TEST_CASE("session: stale position report is discarded")
{
    /* m7.1 phase 3: position reports with a timestamp older than 30 s must
     * be discarded. Staleness compares against real wall-clock time
     * (fss_current_timestamp), so timestamps are built from wall time here. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto clock = std::make_shared<FakeClock>();
    clock->t = 100000; // arbitrary "now" in ms — still injected (harmless)

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->setClock(clock);

    /* Identify (legacy path — no version message, no seq checking). */
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    REQUIRE(handler.disconnects == 0);

    /* Position with a timestamp 60 s in the past — stale. */
    constexpr uint64_t sixty_seconds_ms = 60000;
    auto stale = make_position_msg(fss::fss_current_timestamp() - sixty_seconds_ms);
    session->processMessage(stale);
    REQUIRE(handler.broadcasts.empty());

    /* Position with a current timestamp — fresh. */
    auto fresh = make_position_msg(fss::fss_current_timestamp());
    session->processMessage(fresh);
    REQUIRE(handler.broadcasts.size() == 1);
}

TEST_CASE("session: no-fix (NaN) position report is discarded, not stored or broadcast")
{
    /* Phase 07: a client with no GPS fix sends a NaN position. It decodes to
     * NaN (the wire sentinel), and must be discarded distinctly from a
     * malformed coordinate — never stored or broadcast as a real position. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    REQUIRE(handler.disconnects == 0);

    fss_test::capture_cerr cap;
    auto no_fix = std::make_shared<fss::transport::fss_message_position_report>(
        std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN(), 0U, 0U, 0U, int16_t{0}, 0U,
        std::string{}, 0U, uint8_t{0}, 0U, uint8_t{0}, uint8_t{0}, fss::fss_current_timestamp());
    session->processMessage(no_fix);

    REQUIRE(mock.positions.empty());     // not stored
    REQUIRE(handler.broadcasts.empty()); // not broadcast
    /* Logged distinctly as a no-fix, not the generic invalid-coordinate path. */
    REQUIRE(cap.str().find("no GPS fix") != std::string::npos);

    /* A subsequent real fix is still accepted, proving the session is healthy. */
    auto fresh = make_position_msg(fss::fss_current_timestamp());
    session->processMessage(fresh);
    REQUIRE(handler.broadcasts.size() == 1);
}

TEST_CASE("smm_settings: ctor and accessors")
{
    fss::server::smm_settings s("https://smm.example", fss::secure_string{"user"}, fss::secure_string{"pass"});
    REQUIRE(s.getAddress() == "https://smm.example");
    REQUIRE(s.getUsername() == "user");
    REQUIRE(s.getPassword() == "pass");
}

TEST_CASE("fss_server_details: ctor and accessors")
{
    fss::server::fss_server_details d("10.0.0.1", uint16_t{8080});
    REQUIRE(d.getAddress() == "10.0.0.1");
    REQUIRE(d.getPort() == 8080);
}

TEST_CASE("asset_command: all command-string branches and getAltitude")
{
    using fss::transport::asset_command_goto;
    using fss::transport::asset_command_resume;
    using fss::transport::asset_command_disarm;
    using fss::transport::asset_command_altitude;
    using fss::transport::asset_command_terminate;
    using fss::transport::asset_command_manual;
    using fss::transport::asset_command_unknown;

    auto cmd_goto = fss::server::asset_command(1, 100, "GOTO", 1.0, 2.0, 0);
    REQUIRE(cmd_goto.getCommand() == asset_command_goto);

    auto cmd_ron = fss::server::asset_command(2, 100, "RON", 0.0, 0.0, 0);
    REQUIRE(cmd_ron.getCommand() == asset_command_resume);

    auto cmd_disarm = fss::server::asset_command(3, 100, "DISARM", 0.0, 0.0, 0);
    REQUIRE(cmd_disarm.getCommand() == asset_command_disarm);

    auto cmd_alt = fss::server::asset_command(4, 100, "ALT", 0.0, 0.0, uint32_t{150});
    REQUIRE(cmd_alt.getCommand() == asset_command_altitude);
    REQUIRE(cmd_alt.getAltitude() == 150);

    auto cmd_term = fss::server::asset_command(5, 100, "TERM", 0.0, 0.0, 0);
    REQUIRE(cmd_term.getCommand() == asset_command_terminate);

    auto cmd_man = fss::server::asset_command(6, 100, "MAN", 0.0, 0.0, 0);
    REQUIRE(cmd_man.getCommand() == asset_command_manual);

    auto cmd_bad = fss::server::asset_command(7, 100, "BADCMD", 0.0, 0.0, 0);
    REQUIRE(cmd_bad.getCommand() == asset_command_unknown);
}

TEST_CASE("session: sendCommand dispatches altitude message for ALT command")
{
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 10;
    mock.asset_ids["craft"] = asset_id;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    conn->sent.clear();

    auto alt_cmd = std::make_shared<fss::server::asset_command>(42, 1000, "ALT", 0.0, 0.0, uint32_t{300});
    session->setPendingCommand(alt_cmd);
    session->sendCommand();

    auto cmd_msg = find_sent<fss::transport::fss_message_asset_command>(conn->sent);
    REQUIRE(cmd_msg != nullptr);
    REQUIRE(cmd_msg->getCommand() == fss::transport::asset_command_altitude);
}

TEST_CASE("session: sendCommand skips unknown command type")
{
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 11;
    mock.asset_ids["craft"] = asset_id;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    conn->sent.clear();

    auto bad_cmd = std::make_shared<fss::server::asset_command>(99, 2000, "BADCMD", 0.0, 0.0, 0);
    session->setPendingCommand(bad_cmd);
    session->sendCommand();

    REQUIRE(find_sent<fss::transport::fss_message_asset_command>(conn->sent) == nullptr);
}

TEST_CASE("session: sendCommand refuses GOTO built from a NULL position")
{
    /* A command row with a NULL position maps to NaN coordinates in the DB
     * layer (db_asset_command_get); the dispatch guard must refuse it rather
     * than send the aircraft to whatever the host vars happened to hold. */
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 21;
    mock.asset_ids["craft"] = asset_id;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    conn->sent.clear();

    auto null_pos_goto = std::make_shared<fss::server::asset_command>(100, 2000, "GOTO", NAN, NAN, 0);
    session->setPendingCommand(null_pos_goto);
    session->sendCommand();

    REQUIRE(find_sent<fss::transport::fss_message_asset_command>(conn->sent) == nullptr);
}

TEST_CASE("session: sendCommand refuses ALT built from a NULL altitude")
{
    /* A command row with a NULL altitude must not dispatch as altitude 0 -
     * that is a descend-to-ground instruction. The DB layer flags the NULL
     * via altitude_valid=false. */
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 22;
    mock.asset_ids["craft"] = asset_id;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    conn->sent.clear();

    auto null_alt =
        std::make_shared<fss::server::asset_command>(101, 2000, "ALT", 0.0, 0.0, 0, /*altitude_valid*/ false);
    session->setPendingCommand(null_alt);
    session->sendCommand();
    REQUIRE(find_sent<fss::transport::fss_message_asset_command>(conn->sent) == nullptr);

    /* The same command with a real altitude dispatches normally. */
    auto valid_alt = std::make_shared<fss::server::asset_command>(102, 2001, "ALT", 0.0, 0.0, 250);
    session->setPendingCommand(valid_alt);
    session->sendCommand();
    auto cmd_msg = find_sent<fss::transport::fss_message_asset_command>(conn->sent);
    REQUIRE(cmd_msg != nullptr);
    REQUIRE(cmd_msg->getCommand() == fss::transport::asset_command_altitude);
    REQUIRE(cmd_msg->getAltitude() == 250);
}

TEST_CASE("session: sendSMMSettings sends smm_settings message when db returns settings")
{
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 12;
    mock.asset_ids["craft"] = asset_id;
    mock.smm[asset_id] = std::make_shared<fss::server::smm_settings>("https://smm.test", fss::secure_string{"u"},
                                                                     fss::secure_string{"p"});

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    REQUIRE(find_sent<fss::transport::fss_message_smm_settings>(conn->sent) != nullptr);
}

TEST_CASE("MonotonicClock: now_ms is non-decreasing")
{
    /* Sanity check: two back-to-back calls must return a non-decreasing
     * value — CLOCK_MONOTONIC never goes backward. */
    fss::MonotonicClock mc;
    uint64_t t1 = mc.now_ms();
    uint64_t t2 = mc.now_ms();
    REQUIRE(t2 >= t1);
}

TEST_CASE("session: system_status message is forwarded to db writer")
{
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 13;
    mock.asset_ids["craft"] = asset_id;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    auto status =
        std::make_shared<fss::transport::fss_message_system_status>(uint8_t{80}, uint32_t{1000}, double{12.5});
    session->processMessage(status);

    REQUIRE(fss_test::wait_for([&]() { return !mock.statuses.empty(); }));
    REQUIRE(mock.statuses.front().asset_id == asset_id);
    REQUIRE(mock.statuses.front().bat_percent == 80);
}

TEST_CASE("session: search_status message is forwarded to db writer")
{
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 14;
    mock.asset_ids["craft"] = asset_id;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    auto search = std::make_shared<fss::transport::fss_message_search_status>(uint64_t{5}, uint64_t{3}, uint64_t{10});
    session->processMessage(search);

    REQUIRE(fss_test::wait_for([&]() { return !mock.searches.empty(); }));
    REQUIRE(mock.searches.front().asset_id == asset_id);
    REQUIRE(mock.searches.front().search_id == 5);
    REQUIRE(mock.searches.front().completed == 3);
    REQUIRE(mock.searches.front().total == 10);
}

TEST_CASE("session: rtt_request from client triggers rtt_response reply")
{
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    conn->sent.clear();

    auto rtt_req = std::make_shared<fss::transport::fss_message_rtt_request>();
    session->processMessage(rtt_req);

    REQUIRE(find_sent<fss::transport::fss_message_rtt_response>(conn->sent) != nullptr);
}

TEST_CASE("session: position report with out-of-range latitude is rejected")
{
    /* A position report whose latitude is outside [-90, 90] must be silently
     * dropped: it must neither be written to the database nor broadcast to
     * other connected clients. The connection itself stays up. */
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 20;
    mock.asset_ids["craft"] = asset_id;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    REQUIRE(handler.disconnects == 0);

    /* Send a position report with latitude=200.0 (well outside [-90,90]). */
    auto bad_pos = std::make_shared<fss::transport::fss_message_position_report>(
        200.0, 0.0, 0U, 0U, 0U, int16_t{0}, 0U, std::string{}, 0U, uint8_t{0}, 0U, uint8_t{0}, uint8_t{0},
        fss::fss_current_timestamp());
    fss_test::capture_cerr cap;
    session->processMessage(bad_pos);

    /* Must not be broadcast. */
    REQUIRE(handler.broadcasts.empty());
    /* Must not be written to the database. */
    REQUIRE(mock.positions.empty());
    /* Connection must stay up. */
    REQUIRE(handler.disconnects == 0);
    /* A warning must be logged. */
    REQUIRE(cap.str().find("Invalid position report coordinates") != std::string::npos);

    /* A follow-up report with valid coordinates must be accepted. */
    auto good_pos = std::make_shared<fss::transport::fss_message_position_report>(
        -43.5, 172.6, 100U, 0U, 0U, int16_t{0}, 0U, std::string{}, 0U, uint8_t{0}, 0U, uint8_t{0}, uint8_t{0},
        fss::fss_current_timestamp());
    session->processMessage(good_pos);

    REQUIRE(handler.broadcasts.size() == 1);
    REQUIRE(fss_test::wait_for([&]() { return !mock.positions.empty(); }));
    REQUIRE(mock.positions.front().asset_id == asset_id);
}

TEST_CASE("session: position report with invalid longitude or non-finite coords is rejected")
{
    /* is_valid_coordinate also rejects out-of-range longitude and non-finite
     * lat/long; each such report must be neither stored nor broadcast. */
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 22;
    mock.asset_ids["craft"] = asset_id;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    auto make_pos = [](double lat, double lng) -> std::shared_ptr<fss::transport::fss_message_position_report> {
        return std::make_shared<fss::transport::fss_message_position_report>(
            lat, lng, 0U, 0U, 0U, int16_t{0}, 0U, std::string{}, 0U, uint8_t{0}, 0U, uint8_t{0}, uint8_t{0},
            fss::fss_current_timestamp());
    };

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const auto &bad : {make_pos(0.0, 200.0),  // longitude > 180
                            make_pos(0.0, -181.0), // longitude < -180
                            make_pos(nan, 0.0),    // non-finite latitude
                            make_pos(0.0, inf)})   // non-finite longitude
    {
        session->processMessage(bad);
    }

    REQUIRE(handler.broadcasts.empty());
    REQUIRE(mock.positions.empty());
    REQUIRE(handler.disconnects == 0);
}

TEST_CASE("session: unidentified client receives identity_required for non-identity message")
{
    fss_test::MockDatabase mock;

    auto conn = std::make_shared<FakeConnection>();
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    /* Send any non-identity message before the client has identified.
     * The server must reply with identity_required rather than crashing. */
    session->processMessage(std::make_shared<fss::transport::fss_message_rtt_request>());

    REQUIRE(find_sent<fss::transport::fss_message_identity_required>(conn->sent) != nullptr);
}

TEST_CASE("rate limiter: sustained rate-limiting logs a warning after 1 s")
{
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto clock = std::make_shared<FakeClock>();
    /* Start at non-zero time so the first rate-limited drop sets
     * last_rate_limit_log_ms to a non-zero value (0 is the sentinel
     * for "never dropped"). */
    clock->advance(1000);
    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->setClock(clock);
    session->setRateLimits(1, 0); // 1-message burst, no refill

    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    auto pos = std::make_shared<fss::transport::fss_message_position_report>(
        0.0, 0.0, 0U, 0U, 0U, int16_t{0}, 0U, std::string{}, 0U, uint8_t{0}, 0U, uint8_t{0}, uint8_t{0}, uint64_t{0});

    /* Consume the single token. */
    session->processMessage(pos);
    /* First drop — sets last_rate_limit_log_ms = 1000 ms. */
    session->processMessage(pos);

    /* Advance past the 1-second warning threshold. */
    clock->advance(1001);

    fss_test::capture_cerr cap;
    /* This drop is > 1 s after the first → warning fires. */
    session->processMessage(pos);

    REQUIRE(cap.str().find("Rate-limiting") != std::string::npos);
    REQUIRE(handler.disconnects == 0);
}

TEST_CASE("session: GOTO command dispatch sends lat/lon asset_command message")
{
    /* The asset_command_goto branch in sendCommand() constructs the message
     * via getLatitude() / getLongitude() — these accessors are otherwise
     * uncovered. */
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 20;
    mock.asset_ids["craft"] = asset_id;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    const std::size_t before = conn->sent.size();
    auto cmd = std::make_shared<fss::server::asset_command>(/*dbid*/ 5, /*ts*/ 200, "GOTO", -43.5, 172.6, uint32_t{0});
    mock.pushCommand(asset_id, cmd);
    session->setPendingCommand(cmd);
    session->sendCommand();

    REQUIRE(find_sent<fss::transport::fss_message_asset_command>(conn->sent, before) != nullptr);
}

TEST_CASE("session: GOTO command with out-of-range latitude is not dispatched")
{
    /* sendCommand() must validate GOTO coordinates via is_valid_coordinate
     * before constructing or sending the message.  A latitude of 200.0 is
     * outside [-90, 90] and must be rejected: no asset_command message must
     * appear on the wire and an error must be logged.
     *
     * The command's dbid is recorded before the guard returns, so an
     * unchanged rejected command is deduplicated on subsequent ticks (no log
     * flooding, since sendCommand runs every command_poll_ms), while a
     * replacement command with a different dbid dispatches immediately. */
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 21;
    mock.asset_ids["craft"] = asset_id;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    const std::size_t before = conn->sent.size();

    /* Latitude 200.0 is well outside [-90, 90]. */
    auto bad_cmd =
        std::make_shared<fss::server::asset_command>(/*dbid*/ 6, /*ts*/ 300, "GOTO", 200.0, 172.6, uint32_t{0});
    session->setPendingCommand(bad_cmd);

    {
        fss_test::capture_cerr cap;
        session->sendCommand();
        /* No asset_command must have been sent, and an error must be logged. */
        REQUIRE(find_sent<fss::transport::fss_message_asset_command>(conn->sent, before) == nullptr);
        REQUIRE(cap.str().find("Refusing to dispatch GOTO command with invalid coordinates") != std::string::npos);
    }

    /* Re-issuing the same rejected command immediately must be deduplicated:
     * the dbid was recorded, so within the retry window sendCommand() neither
     * dispatches nor re-logs. This guards against per-tick log flooding. */
    {
        fss_test::capture_cerr cap;
        session->sendCommand();
        REQUIRE(find_sent<fss::transport::fss_message_asset_command>(conn->sent, before) == nullptr);
        REQUIRE(cap.str().find("Refusing to dispatch GOTO") == std::string::npos);
    }

    /* A follow-up with valid coordinates (different dbid) must still dispatch
     * immediately, without waiting for the 10-second retry window. */
    auto good_cmd =
        std::make_shared<fss::server::asset_command>(/*dbid*/ 7, /*ts*/ 400, "GOTO", -43.5, 172.6, uint32_t{0});
    session->setPendingCommand(good_cmd);
    session->sendCommand();

    REQUIRE(find_sent<fss::transport::fss_message_asset_command>(conn->sent, before) != nullptr);
}

TEST_CASE("session: rtt_request from identified client receives rtt_response")
{
    /* An identified aircraft client that receives an rtt_request must reply
     * with an rtt_response carrying the same message id. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    const std::size_t before = conn->sent.size();
    auto req = std::make_shared<fss::transport::fss_message_rtt_request>();
    req->setId(99);
    session->processMessage(req);

    auto resp = find_sent<fss::transport::fss_message_rtt_response>(conn->sent, before);
    REQUIRE(resp != nullptr);
    REQUIRE(resp->getRequestId() == 99);
    REQUIRE(handler.disconnects == 0);
}

TEST_CASE("session: sendSMMSettings returns early when asset_id is zero")
{
    /* An unidentified client (asset_id==0) has an empty settings cache and
     * must neither send a message nor touch the database. */
    fss_test::MockDatabase mock;
    /* Do NOT add an asset_id entry — the client will remain unidentified
     * so cached_asset_id stays 0 after any processMessage call. */

    auto conn = std::make_shared<FakeConnection>();
    NullClientHandler handler;
    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    const std::size_t before = conn->sent.size();
    session->sendSMMSettings();    /* empty cache → nothing sent */
    session->refreshSmmSettings(); /* asset_id == 0 → early return, no DB read */
    REQUIRE(conn->sent.size() == before);
    REQUIRE(mock.smm_reads == 0);
}

TEST_CASE("session: sendSMMSettings sends from cache without re-reading the database")
{
    /* The main loop calls sendSMMSettings() every 15 s; it must serve the
     * cache primed at identify time (and refreshed by the poller thread),
     * never performing a synchronous DB read itself. */
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 14;
    mock.asset_ids["craft"] = asset_id;
    mock.smm[asset_id] = std::make_shared<fss::server::smm_settings>("https://smm.test", fss::secure_string{"u"},
                                                                     fss::secure_string{"p"});

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    const int reads_after_identify = mock.smm_reads;
    REQUIRE(reads_after_identify >= 1); /* identify primes the cache */

    /* Drop the DB entry: a cache-respecting send still delivers the cached
     * settings and performs no new read. */
    mock.smm.clear();
    conn->sent.clear();
    session->sendSMMSettings();
    REQUIRE(find_sent<fss::transport::fss_message_smm_settings>(conn->sent) != nullptr);
    REQUIRE(mock.smm_reads == reads_after_identify);

    /* refreshSmmSettings (poller path) re-reads: the entry is gone, so the
     * cache empties and nothing further is sent. */
    session->refreshSmmSettings();
    REQUIRE(mock.smm_reads == reads_after_identify + 1);
    conn->sent.clear();
    session->sendSMMSettings();
    REQUIRE(find_sent<fss::transport::fss_message_smm_settings>(conn->sent) == nullptr);
}

TEST_CASE("session: sendRTTRequest skips second request within retry interval")
{
    /* When two RTT requests are enqueued consecutively (clock not advanced),
     * the second call must return early without sending a new request. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto clock = std::make_shared<FakeClock>();
    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->setClock(clock);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    auto rtt1 = std::make_shared<fss::transport::fss_message_rtt_request>();
    auto rtt2 = std::make_shared<fss::transport::fss_message_rtt_request>();
    session->sendRTTRequest(rtt1); /* queued at t=0 */

    /* No clock advance — within the rtt_retry_interval → early return. */
    session->sendRTTRequest(rtt2);

    /* Only one rtt_request should appear on the wire. */
    REQUIRE(count_sent<fss::transport::fss_message_rtt_request>(conn->sent) == 1);
    REQUIRE(handler.disconnects == 0);
}

TEST_CASE("asset_command: altitude above uint16_t max survives pack/decode round-trip")
{
    /* Regression: altitude was stored as uint16_t, silently truncating any
     * value above 65535 before it reached the wire. Verify that a value of
     * 100000 (well above 65535) flows through asset_command -> getAltitude()
     * -> fss_message_asset_command (pack) -> decode without truncation. */
    constexpr uint32_t high_alt = 100000U;

    /* 1. asset_command stores and returns the full 32-bit value. */
    fss::server::asset_command ac(/*dbid*/ 1, /*ts*/ 1000, "ALT", 0.0, 0.0, high_alt);
    REQUIRE(ac.getAltitude() == high_alt);

    /* 2. The wire message is constructed from the getter and survives a
     *    pack -> decode round-trip without truncation. */
    auto msg_out = std::make_shared<fss::transport::fss_message_asset_command>(fss::transport::asset_command_altitude,
                                                                               ac.getTimeStamp(), ac.getAltitude());

    auto bl = msg_out->getPacked();
    auto msg_in = fss::transport::fss_message::decode(bl);

    auto cmd_in = std::dynamic_pointer_cast<fss::transport::fss_message_asset_command>(msg_in);
    REQUIRE(cmd_in != nullptr);
    REQUIRE(cmd_in->getCommand() == fss::transport::asset_command_altitude);
    REQUIRE(cmd_in->getAltitude() == high_alt);

    /* 3. Full dispatch path: session delivers the command with the correct
     *    altitude on the wire. */
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 50;
    mock.asset_ids["craft"] = asset_id;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    conn->sent.clear();

    auto alt_cmd = std::make_shared<fss::server::asset_command>(/*dbid*/ 7, /*ts*/ 2000, "ALT", 0.0, 0.0, high_alt);
    session->setPendingCommand(alt_cmd);
    session->sendCommand();

    auto dispatched = find_sent<fss::transport::fss_message_asset_command>(conn->sent);
    REQUIRE(dispatched != nullptr);
    REQUIRE(dispatched->getCommand() == fss::transport::asset_command_altitude);
    REQUIRE(dispatched->getAltitude() == high_alt);
}
