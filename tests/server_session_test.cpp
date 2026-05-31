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
     * be discarded. The test uses a controllable clock so no real time passes. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto clock = std::make_shared<FakeClock>();
    clock->t = 100000; // arbitrary "now" in ms

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

    auto cmd_alt = fss::server::asset_command(4, 100, "ALT", 0.0, 0.0, uint16_t{150});
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

    auto alt_cmd = std::make_shared<fss::server::asset_command>(42, 1000, "ALT", 0.0, 0.0, uint16_t{300});
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
    auto cmd = std::make_shared<fss::server::asset_command>(/*dbid*/ 5, /*ts*/ 200, "GOTO", -43.5, 172.6, uint16_t{0});
    mock.pushCommand(asset_id, cmd);
    session->setPendingCommand(cmd);
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
    /* sendSMMSettings() guards against asset_id==0 (unidentified client)
     * and must return without calling getSmmSettings on the database. */
    fss_test::MockDatabase mock;
    /* Do NOT add an asset_id entry — the client will remain unidentified
     * so cached_asset_id stays 0 after any processMessage call. */

    auto conn = std::make_shared<FakeConnection>();
    NullClientHandler handler;
    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    const std::size_t before = conn->sent.size();
    session->sendSMMSettings(); /* asset_id == 0 → early return */
    /* No smm_settings message should have been sent. */
    REQUIRE(conn->sent.size() == before);
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
