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

/* The listener's accept callback runs on its worker thread; route the accepted
 * connection to the main thread through the mutex-guarded handoff. */
static fss_test::connection_handoff client_handoff;

TEST_CASE("Client Base")
{
    client_handoff.reset();
    constexpr int listen_port = 20402;
    auto listen = std::make_shared<flight_safety_system::transport_ssl::fss_listen>(
        listen_port, client_handoff.callback(), CA_PUBLIC_FILE, SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
    REQUIRE(listen != nullptr);

    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE,
                                                                                 CLIENT_PUBLIC_FILE);
    REQUIRE(client != nullptr);
    client->connectTo("localhost", listen_port, true);

    REQUIRE(client_handoff.wait() != nullptr);

    client_handoff.reset();
}

TEST_CASE("client: config file not found leaves client with no servers")
{
    fss::client_ssl::fss_client client("/nonexistent/config.json");
    REQUIRE_FALSE(client.isConfigured());
    client.attemptReconnect(); // no-op; must not crash
}

TEST_CASE("client: malformed JSON config leaves client unconfigured")
{
    const char *tmppath = "/tmp/fss_test_client_malformed.json";
    {
        std::ofstream f(tmppath);
        f << R"({"name":"test-asset", "ssl": {)";
    }
    fss::client_ssl::fss_client client(tmppath);
    REQUIRE_FALSE(client.isConfigured());
    std::remove(tmppath);
}

TEST_CASE("client: config file skips server entries with invalid ports")
{
    const char *tmppath = "/tmp/fss_test_client_bad_port.json";
    {
        std::ofstream f(tmppath);
        f << R"({
            "name": "test-asset",
            "ssl": {
                "ca_public_key": "ca.pem",
                "client_private_key": "client.key",
                "client_public_key": "client.pem"
            },
            "servers": [
                {"address": "localhost", "port": 70000}
            ]
        })";
    }
    fss::client_ssl::fss_client client(tmppath);
    REQUIRE_FALSE(client.isConfigured());
    std::remove(tmppath);
}

/* Exposes the protected config setter so a test can drive the programmatic
 * (non-file) configuration path. */
class ProgrammaticClient : public fss::client_ssl::fss_client {
public:
    using fss::client_ssl::fss_client::setAssetName;
};

TEST_CASE("client: isConfigured tracks the programmatic setAssetName + connectTo path")
{
    /* The file ctor is not the only way to configure a client; isConfigured()
     * must also reflect a client built via the setAssetName/connectTo setters. */
    ProgrammaticClient client;
    REQUIRE_FALSE(client.isConfigured()); // nothing set yet
    client.setAssetName("test-asset");
    REQUIRE_FALSE(client.isConfigured());       // a name, but no server yet
    client.connectTo("127.0.0.1", 9999, false); // adds a reconnect entry; no live connect
    REQUIRE(client.isConfigured());             // name + at least one server
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

TEST_CASE("client: handleCommandFrom receives the originating server")
{
    /* A client that overrides the connection-aware handler captures the origin
     * so it can reply to that specific connection (the command-ack path). */
    class OriginTrackingClient : public fss::client_ssl::fss_client {
    public:
        OriginTrackingClient() : fss_client() {}
        OriginTrackingClient(const OriginTrackingClient &) = delete;
        OriginTrackingClient(OriginTrackingClient &&) = delete;
        auto operator=(const OriginTrackingClient &) -> OriginTrackingClient & = delete;
        auto operator=(OriginTrackingClient &&) -> OriginTrackingClient & = delete;
        ~OriginTrackingClient() override = default;
        fss::client_ssl::fss_server *seen_origin{nullptr};
        void handleCommandFrom(const std::shared_ptr<fss::transport::fss_message_asset_command> &,
                               fss::client_ssl::fss_server *origin) override
        {
            seen_origin = origin;
        }
    };
    OriginTrackingClient client;
    auto server = std::make_shared<fss::client_ssl::fss_server>(&client, "localhost", uint16_t{0}, CA_PUBLIC_FILE,
                                                                CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto msg =
        std::make_shared<fss::transport::fss_message_asset_command>(fss::transport::asset_command_rtl, uint64_t{0});
    server->processMessage(msg);
    REQUIRE(client.seen_origin == server.get());
}

TEST_CASE("client: handleCommandFrom default delegates to handleCommand")
{
    /* A client that only overrides the connection-agnostic handler still sees
     * commands: the default handleCommandFrom delegates to handleCommand. */
    TrackingClient client;
    auto server = std::make_shared<fss::client_ssl::fss_server>(&client, "localhost", uint16_t{0}, CA_PUBLIC_FILE,
                                                                CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    auto msg =
        std::make_shared<fss::transport::fss_message_asset_command>(fss::transport::asset_command_hold, uint64_t{0});
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
    REQUIRE(client.isConfigured());
    REQUIRE(client.getAssetName() == "test-asset");
    std::remove(tmppath);
}

TEST_CASE("client: JSON config tcp_user_timeout_ms sets the per-connection send bound (todo/26)")
{
    /* A flight-safety client (cap-fmu) requests a tighter TCP_USER_TIMEOUT
     * than the 30 s default via its config; every server (re)connect reads
     * it through getTcpUserTimeoutMs(). Absent, the default must hold. */
    const char *tmppath = "/tmp/fss_test_client_send_timeout.json";

    SECTION("configured value is exposed")
    {
        {
            std::ofstream f(tmppath);
            f << R"({"name":"test-asset","tcp_user_timeout_ms":5000,)"
              << R"("ssl":{"ca_public_key":"ca.pem","client_private_key":"key.pem","client_public_key":"cert.pem"},)"
              << R"("servers":[{"address":"127.0.0.1","port":9999}]})";
        }
        fss::client_ssl::fss_client client(tmppath);
        REQUIRE(client.getTcpUserTimeoutMs() == 5000);
    }

    SECTION("absent field keeps the 30s default")
    {
        {
            std::ofstream f(tmppath);
            f << R"({"name":"test-asset",)"
              << R"("ssl":{"ca_public_key":"ca.pem","client_private_key":"key.pem","client_public_key":"cert.pem"},)"
              << R"("servers":[{"address":"127.0.0.1","port":9999}]})";
        }
        fss::client_ssl::fss_client client(tmppath);
        REQUIRE(client.getTcpUserTimeoutMs() == fss::transport::default_tcp_user_timeout_ms);
    }

    std::remove(tmppath);
}

TEST_CASE("client: disconnect closes all active server connections")
{
    /* connectTo with connect=true adds a server to the connected list.
     * A subsequent disconnect() must iterate and close it (lines 65-71). */
    client_handoff.reset();
    const uint16_t port = fss_test::pick_port();
    REQUIRE(port != 0);
    auto listen = std::make_shared<flight_safety_system::transport_ssl::fss_listen>(
        port, client_handoff.callback(), CA_PUBLIC_FILE, SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
    REQUIRE(listen != nullptr);

    auto client =
        std::make_shared<fss::client_ssl::fss_client>(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    client->connectTo("localhost", port, true);
    REQUIRE(client_handoff.wait() != nullptr);

    client->disconnect(); /* exercises lines 65-71 */
    client_handoff.reset();
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

TEST_CASE("fss_connection::sendPacked stamps a broadcast and refuses credentials (todo/55)")
{
    /* sendPacked is the byte-clone broadcast path: it copies a shared, pre-packed
     * frame and stamps this connection's next sequence id into the copy. A normal
     * broadcast frame goes out with a stamped id; a credential-bearing
     * smm_settings frame must be refused at runtime (it would otherwise skip the
     * wipeSecure scrub sendMsg does and leak credentials to every recipient). */
    auto conn = std::make_shared<CapturingConnection>();

    auto position = std::make_shared<fss::transport::fss_message_position_report>(
        1.0, 2.0, 50U, 0U, 0U, int16_t{0}, 0U, std::string{}, 0U, uint8_t{0}, 0U, uint8_t{0}, uint8_t{0}, uint64_t{0});
    REQUIRE(conn->sendPacked(position->getPacked()));
    REQUIRE(conn->sent.size() == 1);
    REQUIRE(conn->sent[0]->getType() == fss::transport::message_type_position_report);
    REQUIRE(conn->sent[0]->getId() != 0); // a per-connection id was stamped in

    auto settings = std::make_shared<fss::transport::fss_message_smm_settings>(
        "https://smm.example/", fss::secure_string(std::string_view{"user"}),
        fss::secure_string(std::string_view{"pass"}));
    REQUIRE_FALSE(conn->sendPacked(settings->getPacked())); // refused
    REQUIRE(conn->sent.size() == 1);                        // nothing sent
}
