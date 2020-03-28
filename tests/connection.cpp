#include "config.h"

#define CATCH_CONFIG_MAIN  // This tells Catch to provide a main() - only do this in one cpp file
#ifdef HAVE_CATCH2_CATCH_HPP
#include <catch2/catch.hpp>
#elif HAVE_CATCH_CATCH_HPP
#include <catch/catch.hpp>
#else
#error No catch header
#endif

#include "fss-transport.hpp"
using namespace flight_safety_system;

TEST_CASE("Connection Create (failure)") {
    auto conn = new transport::fss_connection();
    REQUIRE(conn != nullptr);

    REQUIRE(!conn->connectTo("localhost", 1));
    REQUIRE(!conn->connectTo("127.0.0.1", 1));
    REQUIRE(!conn->connectTo("::1", 1));

    REQUIRE(!conn->connectTo("this.host.does.not.exist", 1));

    delete conn;
}

transport::fss_connection *client_conn = nullptr;
bool test_client_connect_cb (transport::fss_connection *new_conn)
{
    client_conn = new_conn;
    return true;
}


TEST_CASE("Listen Socket") {
    auto listen = new transport::fss_listen(20202, test_client_connect_cb);
    REQUIRE(listen != nullptr);

    auto conn = new transport::fss_connection();
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("localhost", 20202));
    auto send_msg = new transport::fss_message_identity("testClient");
    conn->sendMsg(send_msg);

    delete send_msg;
    delete conn;

    sleep(1);

    REQUIRE(client_conn != nullptr);
    auto msg = client_conn->getMsg();

    REQUIRE(msg != nullptr);
    REQUIRE(msg->getType() == transport::message_type_identity);

    delete msg;

    msg = client_conn->getMsg();
    REQUIRE(msg != nullptr);
    REQUIRE(msg->getType() == transport::message_type_closed);

    delete msg;

    delete client_conn;
    client_conn = nullptr;

    delete listen;
}
