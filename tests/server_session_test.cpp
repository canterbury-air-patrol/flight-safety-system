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

#include <condition_variable>
#include <cmath>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
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
    /* sent/send_attempts are written by sendMsg, which the outbound writer
     * thread (todo/21) may run; guard them with sent_lock so a test thread can
     * observe them without racing. Synchronous tests that touch `sent` directly
     * only ever do so on the same thread that called the send, so they stay
     * race-free too. */
    std::vector<std::shared_ptr<fss::transport::fss_message>> sent{};
    std::list<std::string> cert_names{};
    /* When true, sendMsg() reports the socket write failed (as the real
     * transport does on EPIPE / a closed fd). The framed message is still
     * decoded and pushed to send_attempts so a test can count attempts, but it
     * is NOT added to `sent` — it never reached the peer. */
    bool fail_sends{false};
    std::vector<std::shared_ptr<fss::transport::fss_message>> send_attempts{};

    FakeConnection() = default;
    FakeConnection(const FakeConnection &) = delete;
    FakeConnection(FakeConnection &&) = delete;
    auto operator=(const FakeConnection &) -> FakeConnection & = delete;
    auto operator=(FakeConnection &&) -> FakeConnection & = delete;
    ~FakeConnection() override = default;

    auto getClientNames() -> std::list<std::string> override { return cert_names; }

    /* Block (true) or release (false) any in-flight or future sendMsg, modelling
     * a peer whose socket has black-holed. Releasing wakes a blocked writer. */
    void setBlocked(bool b)
    {
        {
            std::scoped_lock guard(this->block_lock);
            this->blocked = b;
        }
        this->block_cv.notify_all();
    }

    /* Thread-safe snapshot of the messages that actually went out. */
    auto sentSnapshot() -> std::vector<std::shared_ptr<fss::transport::fss_message>>
    {
        std::scoped_lock guard(this->sent_lock);
        return this->sent;
    }

    /* Releasing a blocked send is also needed when the connection is torn down,
     * so a worker parked in sendMsg can exit and be joined. */
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
            send_attempts.push_back(msg);
        }
        if (fail_sends)
        {
            return false;
        }
        if (msg != nullptr)
        {
            sent.push_back(msg);
        }
        return true;
    }
private:
    std::mutex sent_lock{};
    std::mutex block_lock{};
    std::condition_variable block_cv{};
    bool blocked{false};
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
                       [&](const fss::server::command_dispatch_write &w) -> void {
                           mock.recordCommandDispatch(w.command_dbid, w.dispatch_id);
                       },
                       [&](const fss::server::command_ack_write &w) -> void {
                           mock.recordCommandAck(w.asset_id, w.dispatch_id, w.ack_state, w.ack_timestamp, w.ack_reason);
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

TEST_CASE("session: identify surfaces a mid-cursor server-list failure instead of a partial list")
{
    /* getActiveServers throws when the cursor is cut short by a mid-iteration
     * error. The throw must propagate out of processMessage (where production
     * wraps it in an exception_guard) rather than a truncated server list
     * reaching the wire. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;
    mock.active_servers.emplace_back("10.0.0.1", uint16_t{8080});
    mock.active_servers_fail = true;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    REQUIRE_THROWS_AS(session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft")),
                      fss::server::database_error);

    /* No server list was sent: the partial result was discarded, not shipped. */
    REQUIRE(find_sent<fss::transport::fss_message_server_list>(conn->sent) == nullptr);
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

TEST_CASE("session: a failed command send records no dispatch and retries on the next tick")
{
    /* todo/19: a socket write that fails must not mark the command dispatched
     * (that would record in the DB a command the aircraft never received) and
     * must not advance the 10 s resend window (the command has to be retried
     * promptly, not after the timeout). */
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 11;
    mock.asset_ids["craft"] = asset_id;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto clock = std::make_shared<FakeClock>();

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->setClock(clock);
    /* Identify with no pending command in the DB, so the command queued below is
     * the first one sendCommand ever attempts. */
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    conn->fail_sends = true;
    auto cmd = std::make_shared<fss::server::asset_command>(/*dbid*/ 77, /*ts*/ 500, "TERM", 0.0, 0.0, 0);
    session->setPendingCommand(cmd);

    const std::size_t attempts_before = conn->send_attempts.size();
    session->sendCommand();
    /* One attempt was made, but the failed write means nothing reached the peer. */
    REQUIRE(conn->send_attempts.size() == attempts_before + 1);
    REQUIRE(count_sent<fss::transport::fss_message_asset_command>(conn->sent) == 0);
    /* Nothing was enqueued on a failed send, so the sink can never observe a
     * dispatch — an immediate check is reliable. */
    REQUIRE(mock.getDispatches().empty());

    /* The clock has not advanced past the 10 s resend window, yet the next tick
     * must retry because the previous attempt never reached the aircraft. */
    session->sendCommand();
    REQUIRE(conn->send_attempts.size() == attempts_before + 2);

    /* Once the socket recovers, the retry goes through and records exactly one
     * dispatch for this command. */
    conn->fail_sends = false;
    session->sendCommand();
    REQUIRE(count_sent<fss::transport::fss_message_asset_command>(conn->sent) == 1);
    REQUIRE(fss_test::wait_for([&]() -> bool { return mock.getDispatches().size() == 1; }));
    REQUIRE(mock.getDispatches().front().command_dbid == 77);
}

TEST_CASE("session: a successful command send records exactly one dispatch and suppresses resends")
{
    /* todo/19: the success path must remain unchanged — one dispatch id is
     * recorded and repeated ticks within the resend window do not resend or
     * re-record. */
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 12;
    mock.asset_ids["craft"] = asset_id;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto clock = std::make_shared<FakeClock>();

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->setClock(clock);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    auto cmd = std::make_shared<fss::server::asset_command>(/*dbid*/ 88, /*ts*/ 500, "RTL", 0.0, 0.0, 0);
    session->setPendingCommand(cmd);
    for (int i = 0; i < 5; ++i)
    {
        session->sendCommand();
    }
    REQUIRE(count_sent<fss::transport::fss_message_asset_command>(conn->sent) == 1);
    REQUIRE(fss_test::wait_for([&]() -> bool { return mock.getDispatches().size() == 1; }));
    REQUIRE(mock.getDispatches().front().command_dbid == 88);
}

TEST_CASE("session: queueCommandSend dispatches a command on the outbound worker thread")
{
    /* todo/21: the server main loop schedules a command via queueCommandSend()
     * and returns immediately; the per-client writer thread performs the actual
     * send. Verify the command does reach the wire via that thread. */
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 21;
    mock.asset_ids["craft"] = asset_id;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    session->activate(); /* starts the outbound writer thread */

    session->setPendingCommand(
        std::make_shared<fss::server::asset_command>(/*dbid*/ 57, /*ts*/ 500, "RTL", 0.0, 0.0, 0));
    session->queueCommandSend();

    REQUIRE(fss_test::wait_for(
        [&]() -> bool { return count_sent<fss::transport::fss_message_asset_command>(conn->sentSnapshot()) == 1; }));

    session->disconnect();
}

TEST_CASE("session: a blocked client's writer does not stall command dispatch for another client")
{
    /* todo/21 Done-when: a black-holed socket on one client must not delay
     * command dispatch for unrelated clients. Block client A's writer mid-send,
     * then dispatch to client B and require B's command to go out regardless. */
    fss_test::MockDatabase mock;
    mock.asset_ids["alpha"] = 1;
    mock.asset_ids["bravo"] = 2;
    NullClientHandler handler;

    auto conn_a = std::make_shared<FakeConnection>();
    conn_a->cert_names.push_back("alpha");
    auto writer_a = make_mock_writer(mock);
    auto client_a = std::make_shared<fss::server::fss_client>(conn_a, &mock, writer_a, &handler);
    client_a->processMessage(std::make_shared<fss::transport::fss_message_identity>("alpha"));
    client_a->activate();

    auto conn_b = std::make_shared<FakeConnection>();
    conn_b->cert_names.push_back("bravo");
    auto writer_b = make_mock_writer(mock);
    auto client_b = std::make_shared<fss::server::fss_client>(conn_b, &mock, writer_b, &handler);
    client_b->processMessage(std::make_shared<fss::transport::fss_message_identity>("bravo"));
    client_b->activate();

    /* A's socket black-holes: its writer thread will park inside send(). */
    conn_a->setBlocked(true);
    client_a->setPendingCommand(
        std::make_shared<fss::server::asset_command>(/*dbid*/ 10, /*ts*/ 500, "RTL", 0.0, 0.0, 0));
    client_a->queueCommandSend();

    /* B's command must still be delivered while A's writer is stuck. */
    client_b->setPendingCommand(
        std::make_shared<fss::server::asset_command>(/*dbid*/ 20, /*ts*/ 500, "HOLD", 0.0, 0.0, 0));
    client_b->queueCommandSend();

    REQUIRE(fss_test::wait_for(
        [&]() -> bool { return count_sent<fss::transport::fss_message_asset_command>(conn_b->sentSnapshot()) == 1; }));
    /* A's command has not gone out — its writer is blocked, not crashed. */
    REQUIRE(count_sent<fss::transport::fss_message_asset_command>(conn_a->sentSnapshot()) == 0);

    /* disconnect() releases the block and joins A's writer cleanly. */
    client_a->disconnect();
    client_b->disconnect();
}

TEST_CASE("session: queueRTTRequest sends an RTT request on the outbound worker thread")
{
    /* todo/21: RTT requests are scheduled on the writer thread too, each client
     * building its own request instance. Verify one reaches the wire. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 3;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    session->activate();

    REQUIRE(count_sent<fss::transport::fss_message_rtt_request>(conn->sentSnapshot()) == 0);
    session->queueRTTRequest();
    REQUIRE(fss_test::wait_for(
        [&]() -> bool { return count_sent<fss::transport::fss_message_rtt_request>(conn->sentSnapshot()) == 1; }));

    session->disconnect();
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

TEST_CASE("session: version handshake negotiates feature flags as the intersection")
{
    /* The negotiated capability set is (peer-advertised & FSS_SUPPORTED_FEATURES):
     * a peer cannot enable a feature this build does not implement, and the
     * value is recorded on the connection for the rest of the session. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    /* No capabilities negotiated until the handshake. */
    REQUIRE(conn->getNegotiatedFeatureFlags() == 0U);

    /* Peer advertises every bit; we must mask down to what we support. */
    constexpr uint32_t all_flags = 0xFFFFFFFFU;
    auto version = std::make_shared<fss::transport::fss_message_version>(
        fss::transport::FSS_PROTOCOL_VERSION, fss::transport::FSS_PROTOCOL_MIN_VERSION, all_flags);
    session->processMessage(version);

    REQUIRE(conn->getNegotiatedFeatureFlags() == (all_flags & fss::transport::FSS_SUPPORTED_FEATURES));
    REQUIRE(handler.disconnects == 0);
}

TEST_CASE("session: legacy peer advertising no feature flags negotiates none")
{
    /* A peer advertising 0 (an old client, or one with no optional features)
     * leaves the negotiated capability set empty regardless of what we support. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    auto version = std::make_shared<fss::transport::fss_message_version>(fss::transport::FSS_PROTOCOL_VERSION,
                                                                         fss::transport::FSS_PROTOCOL_MIN_VERSION, 0U);
    session->processMessage(version);

    REQUIRE(conn->getNegotiatedFeatureFlags() == 0U);
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
                                                                         fss::transport::FSS_PROTOCOL_MIN_VERSION,
                                                                         fss::transport::FSS_SUPPORTED_FEATURES);
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
    /* Capabilities negotiated at handshake; a duplicate must not disturb them. */
    uint32_t negotiated_features = conn->getNegotiatedFeatureFlags();

    auto pos = make_position_msg(fss::fss_current_timestamp());
    pos->setId(next_id++);
    session->processMessage(pos);
    REQUIRE(handler.broadcasts.size() == 1);

    /* In-sequence duplicate offering a lower version and no capabilities: must
     * neither downgrade the version nor clear the negotiated feature flags. */
    auto dup = std::make_shared<fss::transport::fss_message_version>(1U, 1U, 0U);
    dup->setId(next_id++);
    session->processMessage(dup);
    REQUIRE(conn->getNegotiatedVersion() == fss::transport::FSS_PROTOCOL_VERSION);
    REQUIRE(conn->getNegotiatedFeatureFlags() == negotiated_features);
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

    REQUIRE(mock.getPositions().empty()); // not stored
    REQUIRE(handler.broadcasts.empty());  // not broadcast
    /* Logged distinctly as a no-fix, not the generic invalid-coordinate path. */
    REQUIRE(cap.str().find("no GPS fix") != std::string::npos);

    /* A subsequent real fix is still accepted, proving the session is healthy. */
    auto fresh = make_position_msg(fss::fss_current_timestamp());
    session->processMessage(fresh);
    REQUIRE(handler.broadcasts.size() == 1);
}

TEST_CASE("session: future-dated position report is discarded (symmetric window)")
{
    /* Phase 08: the staleness window is symmetric — a client clock ahead of the
     * server is as wrong as one behind. Previously a future timestamp passed
     * the one-sided check unconditionally. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    REQUIRE(handler.disconnects == 0);

    constexpr uint64_t sixty_seconds_ms = 60000;
    auto future = make_position_msg(fss::fss_current_timestamp() + sixty_seconds_ms);
    session->processMessage(future);
    REQUIRE(handler.broadcasts.empty());

    /* An in-window report is still accepted. */
    auto fresh = make_position_msg(fss::fss_current_timestamp());
    session->processMessage(fresh);
    REQUIRE(handler.broadcasts.size() == 1);
}

TEST_CASE("session: position_staleness_ms = 0 disables the staleness gate")
{
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->setStalenessMs(0); // escape hatch for fleets with undisciplined clocks

    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    REQUIRE(handler.disconnects == 0);

    constexpr uint64_t sixty_seconds_ms = 60000;
    auto old = make_position_msg(fss::fss_current_timestamp() - sixty_seconds_ms);
    session->processMessage(old);
    REQUIRE(handler.broadcasts.size() == 1); // accepted despite being well outside the window
}

TEST_CASE("session: RTT clock-offset measurement shifts the staleness gate")
{
    /* Phase 17.3: a client whose clock is skewed but whose link is healthy must
     * not have its positions dropped. An RTT response carrying the client's
     * clock lets the server measure the offset and shift the gate by it. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    /* The peer negotiated the RTT clock-offset capability. */
    conn->setNegotiatedFeatureFlags(fss::transport::FSS_FEATURE_RTT_OFFSET);
    session->setStalenessMs(5000);

    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    REQUIRE(handler.disconnects == 0);

    /* The client's clock runs 60 s ahead of the server — well outside the 5 s
     * window, so absent any correction every report it sends is discarded. */
    constexpr int64_t client_ahead_ms = 60000;

    /* Drive one RTT round trip whose response carries the client's skewed clock.
     * sendMsg assigns the request its id; the response echoes it back. */
    auto rtt_req = std::make_shared<fss::transport::fss_message_rtt_request>();
    session->sendRTTRequest(rtt_req);
    uint64_t client_now = fss::fss_current_timestamp() + static_cast<uint64_t>(client_ahead_ms);
    auto rtt_resp = std::make_shared<fss::transport::fss_message_rtt_response>(rtt_req->getId(), client_now);
    session->processMessage(rtt_resp);

    /* Offset measured at ~ +60 s (generous slop for test wall-clock drift and
     * half-RTT rounding; the round trip here is sub-millisecond). */
    constexpr int64_t slop_ms = 2000;
    REQUIRE(session->getClockOffsetMs() > client_ahead_ms - slop_ms);
    REQUIRE(session->getClockOffsetMs() < client_ahead_ms + slop_ms);

    /* A report stamped with the client's skewed-but-fresh clock is now accepted
     * rather than discarded as stale. */
    auto skewed = make_position_msg(fss::fss_current_timestamp() + static_cast<uint64_t>(client_ahead_ms));
    session->processMessage(skewed);
    REQUIRE(handler.broadcasts.size() == 1);
}

TEST_CASE("session: RTT clock-offset is ignored without the negotiated capability")
{
    /* The offset must only be trusted when the peer negotiated the capability:
     * a stray client timestamp on the wire from a peer that did not negotiate
     * it must leave the gate a plain symmetric window. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    /* No capability negotiated (negotiated feature flags left at 0). */
    session->setStalenessMs(5000);

    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    REQUIRE(handler.disconnects == 0);

    constexpr int64_t client_ahead_ms = 60000;
    auto rtt_req = std::make_shared<fss::transport::fss_message_rtt_request>();
    session->sendRTTRequest(rtt_req);
    uint64_t client_now = fss::fss_current_timestamp() + static_cast<uint64_t>(client_ahead_ms);
    auto rtt_resp = std::make_shared<fss::transport::fss_message_rtt_response>(rtt_req->getId(), client_now);
    session->processMessage(rtt_resp);

    REQUIRE(session->getClockOffsetMs() == 0); // never measured

    /* So the skewed report is still discarded as stale. */
    auto skewed = make_position_msg(fss::fss_current_timestamp() + static_cast<uint64_t>(client_ahead_ms));
    session->processMessage(skewed);
    REQUIRE(handler.broadcasts.empty());
}

TEST_CASE("session: RTT clock-offset ignores samples from a slow link")
{
    /* A sample's error is bounded by half the round trip, so a response that
     * took longer than max_rtt_for_clock_offset_ms (5 s) is too coarse to trust
     * and must not move the offset. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    auto clock = std::make_shared<FakeClock>(); // drives the monotonic RTT timing
    session->setClock(clock);
    conn->setNegotiatedFeatureFlags(fss::transport::FSS_FEATURE_RTT_OFFSET);

    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    auto rtt_req = std::make_shared<fss::transport::fss_message_rtt_request>();
    session->sendRTTRequest(rtt_req); // recorded at monotonic t=0
    clock->advance(6000);             // a 6 s round trip — beyond the 5 s cap
    auto rtt_resp = std::make_shared<fss::transport::fss_message_rtt_response>(rtt_req->getId(),
                                                                               fss::fss_current_timestamp() + 60000);
    session->processMessage(rtt_resp);

    REQUIRE(session->getClockOffsetMs() == 0); // sample dropped, offset untouched
}

TEST_CASE("session: RTT clock-offset smooths across samples")
{
    /* The first sample seeds the offset directly; subsequent samples move it by
     * 1/divisor of the gap (EWMA), so a later sample at a different offset pulls
     * the estimate partway rather than replacing it. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    auto clock = std::make_shared<FakeClock>(); // keep every round trip at 0 ms
    session->setClock(clock);
    conn->setNegotiatedFeatureFlags(fss::transport::FSS_FEATURE_RTT_OFFSET);

    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    /* Sample 1: client 60 s ahead → seeds the offset at ~ +60 s. */
    auto req1 = std::make_shared<fss::transport::fss_message_rtt_request>();
    session->sendRTTRequest(req1);
    auto resp1 =
        std::make_shared<fss::transport::fss_message_rtt_response>(req1->getId(), fss::fss_current_timestamp() + 60000);
    session->processMessage(resp1);

    /* Sample 2: client now matches the server (offset 0). With divisor 4 the
     * estimate moves to 60000 + (0 - 60000)/4 = 45000, not all the way to 0. */
    auto req2 = std::make_shared<fss::transport::fss_message_rtt_request>();
    session->sendRTTRequest(req2);
    auto resp2 =
        std::make_shared<fss::transport::fss_message_rtt_response>(req2->getId(), fss::fss_current_timestamp());
    session->processMessage(resp2);

    constexpr int64_t slop_ms = 2000;
    REQUIRE(session->getClockOffsetMs() > 45000 - slop_ms);
    REQUIRE(session->getClockOffsetMs() < 45000 + slop_ms);
}

TEST_CASE("session: repeated clock skew escalates WARN -> ERROR and resets on recovery")
{
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);

    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    REQUIRE(handler.disconnects == 0);

    constexpr uint64_t sixty_seconds_ms = 60000;
    fss_test::capture_cerr cap;
    /* Ten consecutive stale reports — the escalation fires an ERROR. */
    for (int i = 0; i < 10; ++i)
    {
        session->processMessage(make_position_msg(fss::fss_current_timestamp() - sixty_seconds_ms));
    }
    REQUIRE(handler.broadcasts.empty());
    REQUIRE(cap.str().find("clock skew suspected") != std::string::npos);

    /* An in-window report resets the streak. */
    session->processMessage(make_position_msg(fss::fss_current_timestamp()));
    REQUIRE(handler.broadcasts.size() == 1);

    /* A single subsequent stale report logs only WARN, not another ERROR. */
    cap.clear();
    session->processMessage(make_position_msg(fss::fss_current_timestamp() - sixty_seconds_ms));
    REQUIRE(cap.str().find("Stale position report") != std::string::npos);
    REQUIRE(cap.str().find("clock skew suspected") == std::string::npos);
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

TEST_CASE("session: queueSMMSettings sends cached settings on the outbound worker thread")
{
    /* todo/21: the periodic SMM-settings push is scheduled on the writer thread
     * too. Verify a queued send reaches the wire via that thread. */
    fss_test::MockDatabase mock;
    constexpr uint64_t asset_id = 13;
    mock.asset_ids["craft"] = asset_id;
    mock.smm[asset_id] = std::make_shared<fss::server::smm_settings>("https://smm.test", fss::secure_string{"u"},
                                                                     fss::secure_string{"p"});

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));
    session->activate();

    /* identify already pushed one set of settings synchronously. */
    const std::size_t before = count_sent<fss::transport::fss_message_smm_settings>(conn->sentSnapshot());
    session->queueSMMSettings();
    REQUIRE(fss_test::wait_for([&]() -> bool {
        return count_sent<fss::transport::fss_message_smm_settings>(conn->sentSnapshot()) == before + 1;
    }));

    session->disconnect();
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

    REQUIRE(fss_test::wait_for([&]() { return !mock.getStatuses().empty(); }));
    auto statuses = mock.getStatuses();
    REQUIRE(statuses.front().asset_id == asset_id);
    REQUIRE(statuses.front().bat_percent == 80);
}

TEST_CASE("session: command_ack is stored when the capability is negotiated")
{
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 13;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    /* The peer negotiated command-ack, so the server honours the ack. */
    conn->setNegotiatedFeatureFlags(fss::transport::FSS_FEATURE_COMMAND_ACK);
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    constexpr uint64_t acked_id = 0xABCDEF;
    constexpr uint64_t ack_ts = 1750000000000ULL;
    auto ack = std::make_shared<fss::transport::fss_message_command_ack>(acked_id, fss::transport::asset_command_manual,
                                                                         fss::transport::command_ack_superseded,
                                                                         fss::transport::supersede_low_battery, ack_ts);
    session->processMessage(ack);

    REQUIRE(fss_test::wait_for([&]() { return !mock.getAcks().empty(); }));
    /* The ack is scoped to the acking asset (the one this connection identified
     * as), not just the per-connection dispatch_id. */
    auto acks = mock.getAcks();
    REQUIRE(acks.front().asset_id == 13);
    REQUIRE(acks.front().dispatch_id == acked_id);
    REQUIRE(acks.front().ack_state == static_cast<uint8_t>(fss::transport::command_ack_superseded));
    REQUIRE(acks.front().ack_timestamp == ack_ts);
    REQUIRE(acks.front().ack_reason == static_cast<uint8_t>(fss::transport::supersede_low_battery));
}

TEST_CASE("session: command_ack is dropped when the capability is not negotiated")
{
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 13;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    /* No negotiated flags: a conforming client never sends an ack, so one that
     * arrives is from a misbehaving peer and must be ignored. */
    NullClientHandler handler;

    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    auto ack = std::make_shared<fss::transport::fss_message_command_ack>(
        uint64_t{0xABCDEF}, fss::transport::asset_command_rtl, fss::transport::command_ack_actioned, uint64_t{1});
    session->processMessage(ack);

    /* Give the async writer a brief chance to run, then confirm nothing was
     * stored. A short timeout suffices: a real write would land near-instantly. */
    REQUIRE_FALSE(fss_test::wait_for([&]() { return !mock.getAcks().empty(); }, std::chrono::milliseconds(200)));
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

    REQUIRE(fss_test::wait_for([&]() { return !mock.getSearches().empty(); }));
    auto searches = mock.getSearches();
    REQUIRE(searches.front().asset_id == asset_id);
    REQUIRE(searches.front().search_id == 5);
    REQUIRE(searches.front().completed == 3);
    REQUIRE(searches.front().total == 10);
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
    REQUIRE(mock.getPositions().empty());
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
    REQUIRE(fss_test::wait_for([&]() { return !mock.getPositions().empty(); }));
    REQUIRE(mock.getPositions().front().asset_id == asset_id);
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
    REQUIRE(mock.getPositions().empty());
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

TEST_CASE("session: a failed RTT send creates no outstanding request and is not throttled")
{
    /* todo/22: a failed RTT write must not create a phantom outstanding request.
     * If it did, the retry throttle would suppress the next request and the
     * liveness timeout would later fire against a local send failure rather than
     * genuine peer silence. The chosen failure policy disconnects the client. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 5;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;
    auto clock = std::make_shared<FakeClock>();
    auto writer = make_mock_writer(mock);
    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, writer, &handler);
    session->setClock(clock);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    conn->fail_sends = true;
    const std::size_t attempts_before = conn->send_attempts.size();

    auto rtt1 = std::make_shared<fss::transport::fss_message_rtt_request>();
    session->sendRTTRequest(rtt1);
    REQUIRE(conn->send_attempts.size() == attempts_before + 1);
    /* Failure policy: the broken connection is reaped immediately. */
    REQUIRE(handler.disconnects > 0);

    /* No outstanding request was recorded, so a second request issued within the
     * rtt_retry_interval (clock not advanced) is NOT throttled — it is attempted
     * again rather than silently suppressed. */
    auto rtt2 = std::make_shared<fss::transport::fss_message_rtt_request>();
    session->sendRTTRequest(rtt2);
    REQUIRE(conn->send_attempts.size() == attempts_before + 2);
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
