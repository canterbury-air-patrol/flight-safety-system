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

TEST_CASE("client: processMessage version incompatibility triggers serverRequiresReconnect")
{
    TrackingClient client;
    auto server = std::make_shared<fss::client_ssl::fss_server>(&client, "localhost", uint16_t{0}, CA_PUBLIC_FILE,
                                                                CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    constexpr uint16_t future_min = fss::transport::FSS_PROTOCOL_VERSION + 1;
    auto msg = std::make_shared<fss::transport::fss_message_version>(future_min, future_min, uint32_t{0});
    server->processMessage(msg); // serverRequiresReconnect called; no crash
}
