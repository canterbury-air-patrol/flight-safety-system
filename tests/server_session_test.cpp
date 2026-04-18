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

TEST_CASE("session: rejects identify when asset unknown to database",
          "[!shouldfail][todo16]")
{
    /* Desired behaviour: if IDatabase::getAssetId returns 0 at identify,
     * the session must not send SMM settings, server list, or commands
     * to the peer. Currently server.cpp accepts the identity and only
     * the silent-drop happens inside DB methods, so the peer still sees
     * application traffic — [!shouldfail] captures the target contract
     * while we refactor. */
    fss_test::MockDatabase mock;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("unknownAsset");
    NullClientHandler handler;

    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, &handler);
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

    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, &handler);
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

TEST_CASE("session: RTT timeout disconnects client",
          "[!shouldfail][todo17]")
{
    /* Pending todo/17: once ClientSession accepts an IClock seam, the
     * test will advance time past the RTT timeout and assert that
     * clientDisconnected fires. The production code has no timeout
     * logic today, so this fails by design. */
    fss_test::MockDatabase mock;
    mock.asset_ids["craft"] = 1;

    auto conn = std::make_shared<FakeConnection>();
    conn->cert_names.push_back("craft");
    NullClientHandler handler;

    auto session = std::make_shared<fss::server::fss_client>(conn, &mock, &handler);
    session->processMessage(std::make_shared<fss::transport::fss_message_identity>("craft"));

    auto rtt_req = std::make_shared<fss::transport::fss_message_rtt_request>();
    session->sendRTTRequest(rtt_req);

    /* No clock seam yet: simulate "time past RTT timeout" by asserting
     * the disconnect callback has fired. It has not. */
    REQUIRE(handler.disconnects > 0);
}
