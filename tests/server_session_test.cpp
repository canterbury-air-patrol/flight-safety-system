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

TEST_CASE("session: a freshly queued command is delivered on the very next sendCommand")
{
    /* todo09: the latency claim is "next 100ms tick picks it up". Model
     * that by pushing a command AFTER identify, then issuing a single
     * sendCommand and asserting the message appears immediately. */
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

    mock.pushCommand(asset_id, std::make_shared<fss::server::asset_command>(
        /*dbid*/ 42, /*ts*/ 500, "DISARM", 0.0, 0.0, 0));
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
