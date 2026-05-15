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

#include <atomic>
#include <memory>

#include "fss-transport-ssl.hpp"
#include "fss-server.hpp"
#include "server-clients.hpp"
#include "db-write-queue.hpp"
#include "mock_database.hpp"
#include "test_helpers.hpp"

namespace fss = flight_safety_system;

namespace {

constexpr const char *CA_PUBLIC_FILE = "certs/ca.public.pem";
constexpr const char *SERVER_PRIVATE_FILE = "certs/localhost.private.pem";
constexpr const char *SERVER_PUBLIC_FILE = "certs/localhost.public.pem";
constexpr const char *CLIENT_PRIVATE_FILE = "certs/client.private.pem";
constexpr const char *CLIENT_PUBLIC_FILE = "certs/client.public.pem";
constexpr const char *CLIENT_CRL_FILE = "certs/client.crl.pem";

auto make_writer(fss_test::MockDatabase &mock) -> std::shared_ptr<fss::server::db_write_queue>
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

} // namespace

TEST_CASE("ssl: disconnectRevokedClients terminates sessions with revoked certs")
{
    const uint16_t port = fss_test::pick_port();
    REQUIRE(port != 0);

    fss_test::MockDatabase mock;
    auto writer = make_writer(mock);
    auto clients = std::make_shared<server_clients>();

    std::atomic<bool> client_registered{false};
    auto connect_cb = [&clients, &mock, &writer,
                       &client_registered](std::shared_ptr<fss::transport::fss_connection> conn) -> bool {
        clients->clientConnected(
            std::make_shared<fss::server::fss_client>(std::move(conn), &mock, writer, clients.get()));
        client_registered = true;
        return true;
    };

    auto listen = std::make_shared<fss::transport_ssl::fss_listen>(port, connect_cb, CA_PUBLIC_FILE,
                                                                   SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
    REQUIRE(listen != nullptr);

    auto client = fss::transport_ssl::fss_connection_client::create(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE,
                                                                    CLIENT_PUBLIC_FILE, "localhost", port);
    REQUIRE(client != nullptr);

    REQUIRE(fss_test::wait_for([&] { return client_registered.load(); }));

    clients->disconnectRevokedClients(CLIENT_CRL_FILE);

    REQUIRE(fss_test::wait_for([&] {
        auto msg = client->getMsg();
        return msg != nullptr && msg->getType() == fss::transport::message_type_closed;
    }));
}
