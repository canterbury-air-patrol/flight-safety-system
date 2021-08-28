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

constexpr const char * CA_PUBLIC_FILE = "certs/ca.public.pem";
constexpr const char * SERVER_PRIVATE_FILE = "certs/server.private.pem";
constexpr const char * SERVER_PUBLIC_FILE = "certs/server.public.pem";
constexpr const char * CLIENT_PRIVATE_FILE = "certs/client.private.pem";
constexpr const char * CLIENT_PUBLIC_FILE = "certs/client.public.pem";


TEST_CASE("SSL - Connection Create (failure)") {
    auto conn = std::make_shared<flight_safety_system::transport_ssl::fss_connection_client>(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    REQUIRE(conn != nullptr);

    REQUIRE(!conn->connectTo("localhost", 1));
    REQUIRE(!conn->connectTo("127.0.0.1", 1));
    REQUIRE(!conn->connectTo("::1", 1));

    REQUIRE(!conn->connectTo("this.host.does.not.exist", 1));
}

static std::shared_ptr<flight_safety_system::transport::fss_connection> client_conn = nullptr;
static auto test_client_connect_cb (std::shared_ptr<flight_safety_system::transport::fss_connection> new_conn) -> bool
{
    client_conn = std::move(new_conn);
    return true;
}


TEST_CASE("SSL - Listen Socket") {
    constexpr int listen_port = 20302;
    auto listen = std::make_shared<flight_safety_system::transport_ssl::fss_listen>(listen_port, test_client_connect_cb, CA_PUBLIC_FILE, SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
    REQUIRE(listen != nullptr);

    std::shared_ptr<flight_safety_system::transport::fss_connection> conn = std::make_shared<flight_safety_system::transport_ssl::fss_connection_client>(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("localhost", listen_port));
    auto send_msg = std::make_shared<flight_safety_system::transport::fss_message_identity>("testClient");
    conn->sendMsg(send_msg);

    sleep(1);

    conn = nullptr;

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
}

class test_ssl_message_cb: public flight_safety_system::transport::fss_message_cb
{
    private:
        std::shared_ptr<flight_safety_system::transport::fss_message> first{};
    public:
        explicit test_ssl_message_cb(std::shared_ptr<flight_safety_system::transport::fss_connection> t_conn) : fss_message_cb(std::move(t_conn)) {};
        auto getFirstMsg() -> std::shared_ptr<flight_safety_system::transport::fss_message> {
            return this->first;
        }
        void processMessage(std::shared_ptr<flight_safety_system::transport::fss_message> message) override {
            this->first = std::move(message);
        }
};


TEST_CASE("SSL - Listen - Callback")
{
    constexpr int listen_port = 20303;

    auto listen = std::make_shared<flight_safety_system::transport_ssl::fss_listen>(listen_port, test_client_connect_cb, CA_PUBLIC_FILE, SERVER_PRIVATE_FILE, SERVER_PUBLIC_FILE);
    REQUIRE(listen != nullptr);

    flight_safety_system::transport::fss_connection *conn = new flight_safety_system::transport_ssl::fss_connection_client(CA_PUBLIC_FILE, CLIENT_PRIVATE_FILE, CLIENT_PUBLIC_FILE);
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("localhost", listen_port));

    conn->sendMsg(std::make_shared<flight_safety_system::transport::fss_message_identity>("testClient"));

    sleep (1);

    REQUIRE(client_conn != nullptr);

    auto cb = std::make_shared<test_ssl_message_cb>(client_conn);
    REQUIRE(cb->connected());
    REQUIRE(cb->getConnection() == client_conn);
    client_conn->setHandler(cb.get());

    REQUIRE(client_conn->getMsg() == nullptr);

    cb->sendMsg(std::make_shared<flight_safety_system::transport::fss_message_rtt_request>());

    sleep(1);

    REQUIRE(cb->getFirstMsg() != nullptr);

    cb->disconnect();
    REQUIRE(!cb->connected());

    client_conn = nullptr;
}
