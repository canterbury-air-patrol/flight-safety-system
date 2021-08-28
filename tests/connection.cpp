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

TEST_CASE("Connection Create (failure)") {
    auto conn = std::make_shared<flight_safety_system::transport::fss_connection>();
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


TEST_CASE("Listen Socket") {
    constexpr int listen_port = 20202;
    auto listen = std::make_shared<flight_safety_system::transport::fss_listen>(listen_port, test_client_connect_cb);
    REQUIRE(listen != nullptr);

    auto conn = std::make_shared<flight_safety_system::transport::fss_connection>();
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("localhost", listen_port));
    auto send_msg = std::make_shared<flight_safety_system::transport::fss_message_identity>("testClient");
    conn->sendMsg(send_msg);

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

class test_message_cb: public flight_safety_system::transport::fss_message_cb
{
    private:
        std::shared_ptr<flight_safety_system::transport::fss_message> first{};
    public:
        explicit test_message_cb(std::shared_ptr<flight_safety_system::transport::fss_connection> t_conn) : fss_message_cb(std::move(t_conn)) {};
        auto getFirstMsg() -> std::shared_ptr<flight_safety_system::transport::fss_message> {
            return this->first;
        }
        void processMessage(std::shared_ptr<flight_safety_system::transport::fss_message> message) override {
            this->first = std::move(message);
        }
};


TEST_CASE("Listen - Callback")
{
    constexpr int listen_port = 20203;

    auto listen = std::make_shared<flight_safety_system::transport::fss_listen>(listen_port, test_client_connect_cb);
    REQUIRE(listen != nullptr);

    auto conn = std::make_shared<flight_safety_system::transport::fss_connection>();
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("localhost", listen_port));

    conn->sendMsg(std::make_shared<flight_safety_system::transport::fss_message_identity>("testClient"));

    sleep (1);

    REQUIRE(client_conn != nullptr);

    auto cb = std::make_shared<test_message_cb>(client_conn);
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
