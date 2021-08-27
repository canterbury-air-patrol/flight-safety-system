#include <cstddef>
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

static std::shared_ptr<transport::fss_connection> client_conn = nullptr;
static bool test_client_connect_cb (std::shared_ptr<transport::fss_connection> new_conn)
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
    auto send_msg = std::make_shared<transport::fss_message_identity>("testClient");
    conn->sendMsg(send_msg);

    delete conn;

    sleep(1);

    REQUIRE(client_conn != nullptr);
    auto msg = client_conn->getMsg();

    REQUIRE(msg != nullptr);
    REQUIRE(msg->getType() == transport::message_type_identity);

    msg = client_conn->getMsg();
    REQUIRE(msg != nullptr);
    REQUIRE(msg->getType() == transport::message_type_closed);

    msg = client_conn->getMsg();
    REQUIRE(msg == nullptr);

    client_conn = nullptr;

    delete listen;
}

class test_message_cb: public transport::fss_message_cb
{
    private:
        std::shared_ptr<transport::fss_message> first{};
    public:
        test_message_cb(std::shared_ptr<flight_safety_system::transport::fss_connection> t_conn) : fss_message_cb(t_conn) {};
        auto getFirstMsg() -> std::shared_ptr<transport::fss_message> {
            return this->first;
        }
        void processMessage(std::shared_ptr<transport::fss_message> message) override {
            this->first = std::move(message);
        }
};


TEST_CASE("Listen - Callback")
{
    auto listen = new transport::fss_listen(20203, test_client_connect_cb);
    REQUIRE(listen != nullptr);

    auto conn = new transport::fss_connection();
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("localhost", 20203));

    conn->sendMsg(std::make_shared<transport::fss_message_identity>("testClient"));

    sleep (1);

    REQUIRE(client_conn != nullptr);

    auto cb = new test_message_cb(client_conn);
    REQUIRE(cb->connected());
    REQUIRE(cb->getConnection() == client_conn);
    client_conn->setHandler(cb);

    REQUIRE(client_conn->getMsg() == nullptr);

    cb->sendMsg(std::make_shared<transport::fss_message_rtt_request>());

    sleep(1);

    REQUIRE(cb->getFirstMsg() != nullptr);

    cb->disconnect();
    REQUIRE(!cb->connected());

    client_conn = nullptr;

    delete listen;
    delete cb;
    delete conn;
}