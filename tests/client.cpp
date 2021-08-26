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
#include "fss-client.hpp"

using namespace flight_safety_system;

static std::shared_ptr<transport::fss_connection> client_conn = nullptr;
static bool test_client_connect_cb (std::shared_ptr<transport::fss_connection> new_conn)
{
    client_conn = new_conn;
    return true;
}

TEST_CASE("Client Base") {
    auto listen = new transport::fss_listen(20302, test_client_connect_cb);
    REQUIRE(listen != nullptr);

    auto client = new client::fss_client();
    REQUIRE(client != nullptr);
    client->connectTo("localhost", 20302, true);

    sleep (1);

    REQUIRE(client_conn != nullptr);

    client_conn = nullptr;

    delete client;
    delete listen;
}