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

#include "fss-transport-ssl.hpp"

#define CA_PUBLIC_FILE "certs/ca.public.pem"
#define SERVER_PRIVATE_FILE "certs/server.private.pem"
#define SERVER_PUBLIC_FILE "certs/server.public.pem"
#define CLIENT_PRIVATE_FILE "certs/client.private.pem"
#define CLIENT_PUBLIC_FILE "certs/client.public.pem"


TEST_CASE("SSL - Connection Create (failure)") {
    auto conn = new flight_safety_system::transport_ssl::fss_connection_client(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    REQUIRE(conn != nullptr);

    REQUIRE(!conn->connectTo("localhost", 1));
    REQUIRE(!conn->connectTo("127.0.0.1", 1));
    REQUIRE(!conn->connectTo("::1", 1));

    REQUIRE(!conn->connectTo("this.host.does.not.exist", 1));

    delete conn;
}

static std::shared_ptr<flight_safety_system::transport::fss_connection> client_conn = nullptr;
static bool test_client_connect_cb (std::shared_ptr<flight_safety_system::transport::fss_connection> new_conn)
{
    client_conn = new_conn;
    return true;
}


TEST_CASE("SSL - Listen Socket") {
    auto listen = new flight_safety_system::transport_ssl::fss_listen(20302, test_client_connect_cb, CA_PUBLIC_FILE, SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
    REQUIRE(listen != nullptr);

    flight_safety_system::transport::fss_connection *conn = new flight_safety_system::transport_ssl::fss_connection_client(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("localhost", 20302));
    auto send_msg = std::make_shared<flight_safety_system::transport::fss_message_identity>("testClient");
    conn->sendMsg(send_msg);

    sleep (1);

    delete conn;

    sleep(1);

    REQUIRE(client_conn != nullptr);
    auto msg = client_conn->getMsg();

    REQUIRE(msg != nullptr);
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_identity);

    msg = client_conn->getMsg();
    REQUIRE(msg != nullptr);
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_closed);

    msg = client_conn->getMsg();
    REQUIRE(msg == nullptr);

    client_conn = nullptr;

    delete listen;
}

class test_ssl_message_cb: public flight_safety_system::transport::fss_message_cb
{
    private:
        std::shared_ptr<flight_safety_system::transport::fss_message> first{};
    public:
        test_ssl_message_cb(std::shared_ptr<flight_safety_system::transport::fss_connection> t_conn) : fss_message_cb(t_conn) {};
        auto getFirstMsg() -> std::shared_ptr<flight_safety_system::transport::fss_message> {
            return this->first;
        }
        void processMessage(std::shared_ptr<flight_safety_system::transport::fss_message> message) override {
            this->first = std::move(message);
        }
};


TEST_CASE("SSL - Listen - Callback")
{
    auto listen = new flight_safety_system::transport_ssl::fss_listen(20303, test_client_connect_cb, CA_PUBLIC_FILE, SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
    REQUIRE(listen != nullptr);

    flight_safety_system::transport::fss_connection *conn = new flight_safety_system::transport_ssl::fss_connection_client(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("localhost", 20303));

    conn->sendMsg(std::make_shared<flight_safety_system::transport::fss_message_identity>("testClient"));

    sleep (1);

    REQUIRE(client_conn != nullptr);

    auto cb = new test_ssl_message_cb(client_conn);
    REQUIRE(cb->connected());
    REQUIRE(cb->getConnection() == client_conn);
    client_conn->setHandler(cb);

    REQUIRE(client_conn->getMsg() == nullptr);

    cb->sendMsg(std::make_shared<flight_safety_system::transport::fss_message_rtt_request>());

    sleep(1);

    REQUIRE(cb->getFirstMsg() != nullptr);

    cb->disconnect();
    REQUIRE(!cb->connected());

    client_conn = nullptr;

    delete listen;
    delete cb;
    delete conn;
}