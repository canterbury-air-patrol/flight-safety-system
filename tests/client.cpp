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
#include "fss-client.hpp"

static std::shared_ptr<flight_safety_system::transport::fss_connection> client_conn = nullptr;
static auto test_client_connect_cb (std::shared_ptr<flight_safety_system::transport::fss_connection> new_conn) -> bool
{
    client_conn = std::move(new_conn);
    return true;
}

TEST_CASE("Client Base") {
    constexpr int listen_port = 20402;
    auto listen = std::make_shared<flight_safety_system::transport::fss_listen>(listen_port, test_client_connect_cb);
    REQUIRE(listen != nullptr);

    auto client = std::make_shared<flight_safety_system::client::fss_client>();
    REQUIRE(client != nullptr);
    client->connectTo("localhost", listen_port, true);

    sleep (1);

    REQUIRE(client_conn != nullptr);

    client_conn = nullptr;
}
