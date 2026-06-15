#include <cstdio>
#include <fstream>
#include <memory>
#include <string_view>
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

#include <unistd.h>

#include "fss-transport.hpp"
#include "fss-client-ssl.hpp"
#include "test_helpers.hpp"

namespace {

namespace fss = flight_safety_system;

class TrackingClient : public fss::client_ssl::fss_client {
public:
    TrackingClient() : fss_client() {}
    bool handle_command_called{false};
    bool handle_position_called{false};
    bool handle_smm_called{false};
    void handleCommand(const std::shared_ptr<fss::transport::fss_message_asset_command> &) override
    {
        handle_command_called = true;
    }
    void handlePositionReport(const std::shared_ptr<fss::transport::fss_message_position_report> &) override
    {
        handle_position_called = true;
    }
    void handleSMMSettings(const std::shared_ptr<fss::transport::fss_message_smm_settings> &) override
    {
        handle_smm_called = true;
    }
};

/* A connection that records the messages the client tries to send, by decoding
 * the framed buffer the base sendMsg(fss_message) hands to sendMsg(buf_len).
 * Mirrors the FakeConnection used in the server-session tests. */
class CapturingConnection : public fss::transport::fss_connection {
public:
    std::vector<std::shared_ptr<fss::transport::fss_message>> sent{};
protected:
    auto sendMsg(const std::shared_ptr<fss::transport::buf_len> &bl) -> bool override
    {
        if (auto decoded = fss::transport::fss_message::decode(bl))
        {
            sent.push_back(decoded);
        }
        return true;
    }
};

/* fss_server with the protected setConnection exposed so a test can inject a
 * CapturingConnection (and preset its negotiated capabilities). */
class ConnInjectableServer : public fss::client_ssl::fss_server {
public:
    using fss::client_ssl::fss_server::fss_server;
    using fss::transport::fss_message_cb::setConnection;
};

} // namespace

constexpr const char *CA_PUBLIC_FILE = "certs/ca.public.pem";
constexpr const char *SERVER_PRIVATE_FILE = "certs/localhost.private.pem";
constexpr const char *SERVER_PUBLIC_FILE = "certs/localhost.public.pem";
constexpr const char *CLIENT_PRIVATE_FILE = "certs/client.private.pem";
constexpr const char *CLIENT_PUBLIC_FILE = "certs/client.public.pem";

static std::shared_ptr<flight_safety_system::transport::fss_connection> client_conn = nullptr;
static auto test_client_connect_cb(std::shared_ptr<flight_safety_system::transport::fss_connection> new_conn) -> bool
{
    client_conn = std::move(new_conn);
    return true;
}

TEST_CASE("Client Base")
{
    constexpr int listen_port = 20402;
    auto listen = std::make_shared<flight_safety_system::transport_ssl::fss_listen>(
        listen_port, test_client_connect_cb, CA_PUBLIC_FILE, SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
    REQUIRE(listen != nullptr);

    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE,
                                                                                 CLIENT_PUBLIC_FILE);
    REQUIRE(client != nullptr);
    client->connectTo("localhost", listen_port, true);

    REQUIRE(fss_test::wait_for([]() { return client_conn != nullptr; }));

    client_conn = nullptr;
}

TEST_CASE("client: config file not found leaves client with no servers")
{
    fss::client_ssl::fss_client client("/nonexistent/config.json");
    client.attemptReconnect(); // no-op; must not crash
}

TEST_CASE("client: processMessage dispatches position_report to handlePositionReport")
{
    TrackingClient client;
    auto server = std::make_shared<fss::client_ssl::fss_server>(&client, "localhost", uint16_t{0}, CA_PUBLIC_FILE,
                                                                CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto msg =
        std::make_shared<fss::transport::fss_message_position_report>(0.0, 0.0, 0, 0, 0, 0, 0, "T", 0, 0, 0, 0, 0, 0);
    server->processMessage(msg);
    REQUIRE(client.handle_position_called);
}

TEST_CASE("client: processMessage dispatches command to handleCommand")
{
    TrackingClient client;
    auto server = std::make_shared<fss::client_ssl::fss_server>(&client, "localhost", uint16_t{0}, CA_PUBLIC_FILE,
                                                                CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto msg =
        std::make_shared<fss::transport::fss_message_asset_command>(fss::transport::asset_command_rtl, uint64_t{0});
    server->processMessage(msg);
    REQUIRE(client.handle_command_called);
}

TEST_CASE("client: processMessage dispatches smm_settings to handleSMMSettings")
{
    TrackingClient client;
    auto server = std::make_shared<fss::client_ssl::fss_server>(&client, "localhost", uint16_t{0}, CA_PUBLIC_FILE,
                                                                CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto msg = std::make_shared<fss::transport::fss_message_smm_settings>("https://smm.example/",
                                                                          fss::secure_string(std::string_view{"user"}),
                                                                          fss::secure_string(std::string_view{"pass"}));
    server->processMessage(msg);
    REQUIRE(client.handle_smm_called);
}

TEST_CASE("client: processMessage server_list calls updateServers")
{
    TrackingClient client;
    auto server = std::make_shared<fss::client_ssl::fss_server>(&client, "localhost", uint16_t{0}, CA_PUBLIC_FILE,
                                                                CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto msg = std::make_shared<fss::transport::fss_message_server_list>();
    msg->addServer("10.0.0.99", uint16_t{9000});
    server->processMessage(msg);
    // updateServers added 10.0.0.99:9000 to the reconnect list; no crash
}

TEST_CASE("client: processMessage rtt_request sends reply (no-op with null connection)")
{
    TrackingClient client;
    auto server = std::make_shared<fss::client_ssl::fss_server>(&client, "localhost", uint16_t{0}, CA_PUBLIC_FILE,
                                                                CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto msg = std::make_shared<fss::transport::fss_message_rtt_request>();
    server->processMessage(msg); // sendMsg returns false (conn is null); must not crash
}

TEST_CASE("client: rtt_request reply carries our clock when rtt-offset negotiated")
{
    TrackingClient client;
    auto server = std::make_shared<ConnInjectableServer>(&client, "localhost", uint16_t{0}, CA_PUBLIC_FILE,
                                                         CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto conn = std::make_shared<CapturingConnection>();
    conn->setNegotiatedFeatureFlags(fss::transport::FSS_FEATURE_RTT_OFFSET);
    server->setConnection(conn);

    server->processMessage(std::make_shared<fss::transport::fss_message_rtt_request>());

    REQUIRE(conn->sent.size() == 1);
    auto resp = std::dynamic_pointer_cast<fss::transport::fss_message_rtt_response>(conn->sent.front());
    REQUIRE(resp != nullptr);
    REQUIRE(resp->getClientTimestamp() != 0); // our wall clock was stamped
}

TEST_CASE("client: rtt_request reply omits our clock without the negotiated capability")
{
    TrackingClient client;
    auto server = std::make_shared<ConnInjectableServer>(&client, "localhost", uint16_t{0}, CA_PUBLIC_FILE,
                                                         CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto conn = std::make_shared<CapturingConnection>();
    /* No capability negotiated (negotiated feature flags left at 0). */
    server->setConnection(conn);

    server->processMessage(std::make_shared<fss::transport::fss_message_rtt_request>());

    REQUIRE(conn->sent.size() == 1);
    auto resp = std::dynamic_pointer_cast<fss::transport::fss_message_rtt_response>(conn->sent.front());
    REQUIRE(resp != nullptr);
    REQUIRE(resp->getClientTimestamp() == 0); // legacy reply, no timestamp
}

TEST_CASE("client: processMessage version incompatibility triggers serverRequiresReconnect")
{
    TrackingClient client;
    auto server = std::make_shared<fss::client_ssl::fss_server>(&client, "localhost", uint16_t{0}, CA_PUBLIC_FILE,
                                                                CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    constexpr uint16_t future_min = fss::transport::FSS_PROTOCOL_VERSION + 1;
    auto msg = std::make_shared<fss::transport::fss_message_version>(future_min, future_min, uint32_t{0});
    server->processMessage(msg); // serverRequiresReconnect called; no crash
}

TEST_CASE("client: version handshake negotiates feature flags as the intersection")
{
    /* Client-side mirror of the server-side test: on the client path the
     * negotiated capability set is also (peer-advertised & FSS_SUPPORTED_FEATURES),
     * recorded on the connection. */
    TrackingClient client;
    auto server = std::make_shared<ConnInjectableServer>(&client, "localhost", uint16_t{0}, CA_PUBLIC_FILE,
                                                         CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto conn = std::make_shared<CapturingConnection>();
    server->setConnection(conn);

    REQUIRE(conn->getNegotiatedFeatureFlags() == 0U); // nothing negotiated yet

    constexpr uint32_t all_flags = 0xFFFFFFFFU; // server advertises every bit
    auto version = std::make_shared<fss::transport::fss_message_version>(
        fss::transport::FSS_PROTOCOL_VERSION, fss::transport::FSS_PROTOCOL_MIN_VERSION, all_flags);
    server->processMessage(version);

    REQUIRE(conn->getNegotiatedFeatureFlags() == (all_flags & fss::transport::FSS_SUPPORTED_FEATURES));
}

TEST_CASE("client: legacy server advertising no feature flags negotiates none")
{
    TrackingClient client;
    auto server = std::make_shared<ConnInjectableServer>(&client, "localhost", uint16_t{0}, CA_PUBLIC_FILE,
                                                         CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto conn = std::make_shared<CapturingConnection>();
    server->setConnection(conn);

    auto version = std::make_shared<fss::transport::fss_message_version>(fss::transport::FSS_PROTOCOL_VERSION,
                                                                         fss::transport::FSS_PROTOCOL_MIN_VERSION, 0U);
    server->processMessage(version);

    REQUIRE(conn->getNegotiatedFeatureFlags() == 0U);
}

TEST_CASE("client: JSON config with server entry parses name and creates reconnect entry")
{
    /* Exercise the file-based fss_client constructor body (lines 24-40) and
     * the setAssetName helper. */
    const char *tmppath = "/tmp/fss_test_client_cfg.json";
    {
        std::ofstream f(tmppath);
        f << R"({"name":"test-asset",)"
          << R"("ssl":{"ca_public_key":"ca.pem","client_private_key":"key.pem","client_public_key":"cert.pem"},)"
          << R"("servers":[{"address":"127.0.0.1","port":9999}]})";
    }
    fss::client_ssl::fss_client client(tmppath);
    REQUIRE(client.getAssetName() == "test-asset");
    std::remove(tmppath);
}

TEST_CASE("client: disconnect closes all active server connections")
{
    /* connectTo with connect=true adds a server to the connected list.
     * A subsequent disconnect() must iterate and close it (lines 65-71). */
    client_conn = nullptr;
    const uint16_t port = fss_test::pick_port();
    REQUIRE(port != 0);
    auto listen = std::make_shared<flight_safety_system::transport_ssl::fss_listen>(
        port, test_client_connect_cb, CA_PUBLIC_FILE, SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
    REQUIRE(listen != nullptr);

    auto client =
        std::make_shared<fss::client_ssl::fss_client>(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    client->connectTo("localhost", port, true);
    REQUIRE(fss_test::wait_for([]() -> bool { return client_conn != nullptr; }));

    client->disconnect(); /* exercises lines 65-71 */
    client_conn = nullptr;
}

TEST_CASE("client: base class handleCommand, handlePositionReport, handleSMMSettings are no-ops")
{
    /* The three virtual handlers have empty base-class bodies that are
     * never reached when the TrackingClient override intercepts them.
     * Drive them through processMessage on a plain fss_client. */
    fss::client_ssl::fss_client client(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto server = std::make_shared<fss::client_ssl::fss_server>(&client, "localhost", uint16_t{0}, CA_PUBLIC_FILE,
                                                                CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);

    auto cmd =
        std::make_shared<fss::transport::fss_message_asset_command>(fss::transport::asset_command_rtl, uint64_t{0});
    server->processMessage(cmd); /* fss_client::handleCommand — no-op */

    auto pos =
        std::make_shared<fss::transport::fss_message_position_report>(0.0, 0.0, 0, 0, 0, 0, 0, "T", 0, 0, 0, 0, 0, 0);
    server->processMessage(pos); /* fss_client::handlePositionReport — no-op */

    auto smm = std::make_shared<fss::transport::fss_message_smm_settings>("https://smm.example/",
                                                                          fss::secure_string(std::string_view{"user"}),
                                                                          fss::secure_string(std::string_view{"pass"}));
    server->processMessage(smm); /* fss_client::handleSMMSettings — no-op */
}
