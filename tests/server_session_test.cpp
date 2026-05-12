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
        if (msg != nullptr) { sent.push_back(msg); }
        return true;
    }
};

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
            [&](const fss::server::position_write &w) -> void { mock.recordPosition(w.asset_id, w.latitude, w.longitude, w.altitude); },
            [&](const fss::server::status_write &w) -> void { mock.recordStatus(w.asset_id, w.bat_percent, w.bat_mah_used, w.bat_voltage); },
            [&](const fss::server::search_status_write &w) -> void { mock.recordSearchStatus(w.asset_id, w.search_id, w.completed, w.total); },
        }, task);
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
    /* todo15: identity name must match a CN from the peer certificate.
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
    /* todo15: getClientNames() returns empty when the peer presented no
     * cert (or the leaf had no CN). The server must refuse to identify
     * such a connection rather than treating absence of a CN as
     * permission to claim any name. */
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

    bool saw_command = false;
    for (const auto &msg : conn->sent)
    {
        if (msg->getType() == fss::transport::message_type_command)
        {
            saw_command = true;
            REQUIRE(msg->getTimeStamp() == 500);
        }
    }
    REQUIRE(saw_command);
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

    std::shared_ptr<fss::transport::fss_message_server_list> server_list_msg;
    for (const auto &msg : conn->sent)
    {
        if (msg->getType() == fss::transport::message_type_server_list)
        {
            server_list_msg = std::dynamic_pointer_cast<fss::transport::fss_message_server_list>(msg);
        }
    }
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
    /* todo09: the main loop now calls sendCommand every 100ms to drop
     * command delivery latency. That is only safe if repeated calls with
     * the same pending row do not resend the message. Verify idempotency
     * by driving sendCommand for 2s worth of ticks against one
     * queued command and counting command messages on the wire. */
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
    for (int i = 0; i < 2 * ticks_per_sec; ++i) { session->sendCommand(); }

    int command_count = 0;
    for (const auto &msg : conn->sent)
    {
        if (msg->getType() == fss::transport::message_type_command) { ++command_count; }
    }
    REQUIRE(command_count == 1);
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
    session->setPendingCommand(cmd);   // simulate one poller tick
    session->sendCommand();

    bool delivered = false;
    for (std::size_t i = sent_before; i < conn->sent.size(); ++i)
    {
        if (conn->sent[i]->getType() == fss::transport::message_type_command) { delivered = true; }
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
    session->sendRTTRequest(rtt_req1);      // queued at t=0

    clock->advance(30001);                  // past rtt_timeout (30 s)

    auto rtt_req2 = std::make_shared<fss::transport::fss_message_rtt_request>();
    session->sendRTTRequest(rtt_req2);      // triggers timeout → disconnect

    REQUIRE(handler.disconnects > 0);
}

TEST_CASE("session: version handshake stores negotiated version and replies")
{
    /* todo02: version exchange — when the client opens with a version
     * message, the server records the negotiated version on the
     * connection and replies with its own version. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    REQUIRE(conn->getNegotiatedVersion() == fss::transport::FSS_PROTOCOL_VERSION_LEGACY);

    auto version = std::make_shared<fss::transport::fss_message_version>(
        fss::transport::FSS_PROTOCOL_VERSION,
        fss::transport::FSS_PROTOCOL_MIN_VERSION,
        0U);
    session->processMessage(version);

    REQUIRE(conn->getNegotiatedVersion() == fss::transport::FSS_PROTOCOL_VERSION);
    REQUIRE(handler.disconnects == 0);

    bool saw_version_response = false;
    for (const auto &m : conn->sent)
    {
        if (m->getType() == fss::transport::message_type_version) { saw_version_response = true; }
    }
    REQUIRE(saw_version_response);
}

TEST_CASE("session: version handshake rejects incompatible peer")
{
    /* todo02: when the client's max version is below our minimum, the
     * server disconnects rather than continuing in an unsupported
     * dialect. */
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
    session->setRateLimits(100, 0);  // 100-message burst, no refill

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
    session->setRateLimits(5, 10);  // 5-message burst, 10/s refill

    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    auto pos = std::make_shared<fss::transport::fss_message_position_report>(
        0.0, 0.0, 0U, 0U, 0U, int16_t{0}, 0U, std::string{}, 0U, uint8_t{0}, 0U, uint8_t{0}, uint8_t{0}, uint64_t{0});
    for (int i = 0; i < 10; ++i) { session->processMessage(pos); }
    // 5 go through, 5 dropped
    REQUIRE(handler.broadcasts.size() == 5);

    // Advance 1 second → 10 new tokens (capped at 5)
    clock->advance(1000);
    for (int i = 0; i < 10; ++i) { session->processMessage(pos); }
    REQUIRE(handler.broadcasts.size() == 10);
    REQUIRE(handler.disconnects == 0);
}

TEST_CASE("session: legacy client (no version handshake) is accepted")
{
    /* todo02: pre-versioning clients send identity directly. The server
     * must accept this and treat the connection as legacy (version 0). */
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
        if (m->getType() == fss::transport::message_type_server_list) { saw_server_list = true; }
    }
    REQUIRE(saw_server_list);
}

/* Helper: build a minimal position report with a given timestamp. */
namespace {
auto make_position_msg(uint64_t ts) -> std::shared_ptr<fss::transport::fss_message_position_report>
{
    return std::make_shared<fss::transport::fss_message_position_report>(
        0.0, 0.0, 0U, 0U, 0U, int16_t{0}, 0U, std::string{},
        0U, uint8_t{0}, 0U, uint8_t{0}, uint8_t{0}, ts);
}

/* Drive a version + identity exchange over a v2 connection.
 * Returns the next seq the client should use for data messages. */
auto establish_v2_session(
    std::shared_ptr<fss::server::fss_client> &session,
    uint64_t &next_id) -> void
{
    auto version = std::make_shared<fss::transport::fss_message_version>(
        fss::transport::FSS_PROTOCOL_VERSION,
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
     * already processed (replay) must be discarded. */
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
    replay->setId(next_id);  // duplicate seq — replay
    session->processMessage(replay);
    REQUIRE(handler.broadcasts.size() == 1);  // not forwarded
    REQUIRE(handler.disconnects == 0);        // connection stays up
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
    auto version = std::make_shared<fss::transport::fss_message_version>(
        fss::transport::FSS_PROTOCOL_VERSION,
        fss::transport::FSS_PROTOCOL_MIN_VERSION, 0U);
    version->setId(1);
    session->processMessage(version);
    REQUIRE(conn->getNegotiatedVersion() == fss::transport::FSS_PROTOCOL_VERSION);

    /* Send identity with wrong seq (e.g. 5 instead of 2). */
    auto identity = std::make_shared<fss::transport::fss_message_identity>("craft");
    identity->setId(5);  // out-of-order
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
     * be discarded. The test uses a controllable clock so no real time passes. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto clock = std::make_shared<FakeClock>();
    clock->t = 100000;  // arbitrary "now" in ms

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->setClock(clock);

    /* Identify (legacy path — no version message, no seq checking). */
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    REQUIRE(handler.disconnects == 0);

    /* Position with a timestamp 60 s in the past — stale. */
    constexpr uint64_t sixty_seconds_ms = 60000;
    auto stale = make_position_msg(clock->t - sixty_seconds_ms);
    session->processMessage(stale);
    REQUIRE(handler.broadcasts.empty());

    /* Position with a current timestamp — fresh. */
    auto fresh = make_position_msg(clock->t);
    session->processMessage(fresh);
    REQUIRE(handler.broadcasts.size() == 1);
}
