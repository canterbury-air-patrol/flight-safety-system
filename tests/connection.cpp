#include <cstddef>
#include <memory>
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
#include "test_helpers.hpp"

TEST_CASE("Connection Create (failure)")
{
    auto conn = std::make_shared<flight_safety_system::transport::fss_connection>();
    REQUIRE(conn != nullptr);

    REQUIRE(!conn->connectTo("localhost", 1));
    REQUIRE(!conn->connectTo("127.0.0.1", 1));
    REQUIRE(!conn->connectTo("::1", 1));

    REQUIRE(!conn->connectTo("this.host.does.not.exist", 1));
}

TEST_CASE("transport: getClientNames returns empty list for base fss_connection")
{
    flight_safety_system::transport::fss_connection conn;
    REQUIRE(conn.getClientNames().empty());
}

static std::shared_ptr<flight_safety_system::transport::fss_connection> client_conn = nullptr;
static auto test_client_connect_cb(std::shared_ptr<flight_safety_system::transport::fss_connection> new_conn) -> bool
{
    client_conn = std::move(new_conn);
    return true;
}


TEST_CASE("Listen Socket")
{
    constexpr int listen_port = 20202;
    auto listen = std::make_shared<flight_safety_system::transport::fss_listen>(listen_port, test_client_connect_cb);
    REQUIRE(listen != nullptr);

    auto conn = std::make_shared<flight_safety_system::transport::fss_connection>();
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("localhost", listen_port));
    auto send_msg = std::make_shared<flight_safety_system::transport::fss_message_identity>("testClient");
    conn->sendMsg(send_msg);

    conn = nullptr;

    REQUIRE(fss_test::wait_for([]() { return client_conn != nullptr; }));

    std::shared_ptr<flight_safety_system::transport::fss_message> msg;
    REQUIRE(fss_test::wait_for([&]() {
        msg = client_conn->getMsg();
        return msg != nullptr;
    }));
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_identity);

    REQUIRE(fss_test::wait_for([&]() {
        msg = client_conn->getMsg();
        return msg != nullptr;
    }));
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_closed);

    msg = client_conn->getMsg();
    REQUIRE(msg == nullptr);

    client_conn = nullptr;
}

class test_message_cb : public flight_safety_system::transport::fss_message_cb {
private:
    std::shared_ptr<flight_safety_system::transport::fss_message> first{};
public:
    explicit test_message_cb(std::shared_ptr<flight_safety_system::transport::fss_connection> t_conn)
        : fss_message_cb(std::move(t_conn)) {};
    auto getFirstMsg() -> std::shared_ptr<flight_safety_system::transport::fss_message> { return this->first; }
    void processMessage(std::shared_ptr<flight_safety_system::transport::fss_message> message) override
    {
        this->first = std::move(message);
    }
};


class small_queue_listen : public flight_safety_system::transport::fss_listen {
public:
    static constexpr size_t queue_limit = 5;
    small_queue_listen(uint16_t t_port, flight_safety_system::transport::fss_connect_cb t_cb)
        : fss_listen(t_port, std::move(t_cb))
    {
    }
protected:
    auto newConnection(int t_fd) -> std::shared_ptr<flight_safety_system::transport::fss_connection> override
    {
        return flight_safety_system::transport::fss_connection::create(t_fd, queue_limit);
    }
};

TEST_CASE("Queue overflow drops oldest messages")
{
    constexpr size_t extra = 3;
    const auto port = fss_test::pick_port();
    REQUIRE(port != 0);

    auto listen = std::make_shared<small_queue_listen>(port, test_client_connect_cb);
    REQUIRE(listen != nullptr);

    auto conn = std::make_shared<flight_safety_system::transport::fss_connection>();
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("localhost", port));

    REQUIRE(fss_test::wait_for([]() { return client_conn != nullptr; }));

    for (size_t i = 0; i < small_queue_listen::queue_limit + extra; ++i)
    {
        conn->sendMsg(std::make_shared<flight_safety_system::transport::fss_message_rtt_request>());
    }

    REQUIRE(fss_test::wait_for([&]() { return client_conn->getDroppedMessages() >= extra; }));
    REQUIRE(client_conn->getDroppedMessages() == extra);

    size_t count = 0;
    while (client_conn->getMsg() != nullptr)
    {
        ++count;
    }
    REQUIRE(count == small_queue_listen::queue_limit);

    conn = nullptr;
    client_conn = nullptr;
}

TEST_CASE("fss_connection: base isPeerCertRevoked always returns false")
{
    auto conn = std::make_shared<flight_safety_system::transport::fss_connection>();
    REQUIRE_FALSE(conn->isPeerCertRevoked(""));
    REQUIRE_FALSE(conn->isPeerCertRevoked("/any/path.pem"));
}

TEST_CASE("Listen - Callback")
{
    constexpr int listen_port = 20203;

    auto listen = std::make_shared<flight_safety_system::transport::fss_listen>(listen_port, test_client_connect_cb);
    REQUIRE(listen != nullptr);

    auto conn = std::make_shared<flight_safety_system::transport::fss_connection>();
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("localhost", listen_port));

    conn->sendMsg(std::make_shared<flight_safety_system::transport::fss_message_identity>("testClient"));

    REQUIRE(fss_test::wait_for([]() { return client_conn != nullptr; }));

    auto cb = std::make_shared<test_message_cb>(client_conn);
    REQUIRE(cb->connected());
    REQUIRE(cb->getConnection() == client_conn);
    client_conn->setHandler(cb.get());

    REQUIRE(client_conn->getMsg() == nullptr);

    cb->sendMsg(std::make_shared<flight_safety_system::transport::fss_message_rtt_request>());

    REQUIRE(fss_test::wait_for([&]() { return cb->getFirstMsg() != nullptr; }));

    cb->disconnect();
    REQUIRE(!cb->connected());

    client_conn = nullptr;
}
