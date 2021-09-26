#include <memory>
#ifdef HAVE_CATCH2_CATCH_HPP
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

constexpr const char * CA_PUBLIC_FILE = "certs/ca.public.pem";
constexpr const char * SERVER_PRIVATE_FILE = "certs/localhost.private.pem";
constexpr const char * SERVER_PUBLIC_FILE = "certs/localhost.public.pem";
constexpr const char * CLIENT_PRIVATE_FILE = "certs/client.private.pem";
constexpr const char * CLIENT_PUBLIC_FILE = "certs/client.public.pem";

static std::shared_ptr<flight_safety_system::transport::fss_connection> client_conn = nullptr;
static auto test_client_connect_cb (std::shared_ptr<flight_safety_system::transport::fss_connection> new_conn) -> bool
{
    client_conn = std::move(new_conn);
    return true;
}

TEST_CASE("Client Base") {
    constexpr int listen_port = 20402;
    auto listen = std::make_shared<flight_safety_system::transport_ssl::fss_listen>(listen_port, test_client_connect_cb, CA_PUBLIC_FILE, SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
    REQUIRE(listen != nullptr);

    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    REQUIRE(client != nullptr);
    client->connectTo("localhost", listen_port, true);

    sleep (1);

    REQUIRE(client_conn != nullptr);

    client_conn = nullptr;
}
