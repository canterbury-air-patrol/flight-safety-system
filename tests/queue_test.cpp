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
#include <cstdint>
#include <memory>
#include <string>

#include "fss-transport.hpp"
#include "test_helpers.hpp"

using flight_safety_system::transport::fss_connection;
using flight_safety_system::transport::fss_listen;
using flight_safety_system::transport::fss_message;
using flight_safety_system::transport::fss_message_identity;
using flight_safety_system::transport::fss_message_rtt_request;
using flight_safety_system::transport::message_type_closed;
using flight_safety_system::transport::message_type_identity;
using flight_safety_system::transport::message_type_rtt_request;

namespace {

/* The listener runs its accept callback on its own worker thread; hand the
 * accepted connection to the main thread through a mutex-guarded handoff so the
 * two do not race on the shared_ptr (or the connection's state). */
fss_test::connection_handoff handoff;

class large_queue_listen : public fss_listen {
public:
    large_queue_listen(uint16_t t_port, flight_safety_system::transport::fss_connect_cb t_cb)
        : fss_listen(t_port, std::move(t_cb), defer_start_t{})
    {
        /* Start the accept thread only once this derived object is fully
         * constructed; the auto-starting base constructor would let the thread
         * virtual-dispatch into newConnection() before the vptr settles. */
        this->startListening();
    }
protected:
    auto newConnection(int t_fd) -> std::shared_ptr<fss_connection> override
    {
        return fss_connection::create(t_fd, 20000);
    }
};

} // namespace

TEST_CASE("queue: FIFO ordering is preserved across many messages")
{
    handoff.reset();
    constexpr uint16_t port = 20502;
    auto listen = std::make_shared<fss_listen>(port, handoff.callback());
    REQUIRE(listen != nullptr);

    auto client = std::make_shared<fss_connection>();
    REQUIRE(client->connectTo("localhost", port));

    constexpr int count = 1000;
    for (int i = 0; i < count; ++i)
    {
        auto msg = std::make_shared<fss_message_identity>(std::to_string(i));
        REQUIRE(client->sendMsg(msg));
    }
    client = nullptr;

    auto server_side_conn = handoff.wait();
    REQUIRE(server_side_conn != nullptr);

    int received = 0;
    while (received < count)
    {
        auto msg = server_side_conn->getMsg();
        if (msg == nullptr)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (msg->getType() == message_type_closed)
        {
            break;
        }
        REQUIRE(msg->getType() == message_type_identity);
        auto ident = std::dynamic_pointer_cast<fss_message_identity>(msg);
        REQUIRE(ident != nullptr);
        REQUIRE(ident->getName() == std::to_string(received));
        ++received;
    }
    REQUIRE(received == count);

    handoff.reset();
}

TEST_CASE("queue: getMsg returns closed sentinel then nullptr on peer disconnect")
{
    handoff.reset();
    constexpr uint16_t port = 20503;
    auto listen = std::make_shared<fss_listen>(port, handoff.callback());
    REQUIRE(listen != nullptr);

    auto client = std::make_shared<fss_connection>();
    REQUIRE(client->connectTo("localhost", port));
    REQUIRE(client->sendMsg(std::make_shared<fss_message_identity>("client1")));

    auto server_side_conn = handoff.wait();
    REQUIRE(server_side_conn != nullptr);

    client = nullptr;

    std::shared_ptr<fss_message> first;
    REQUIRE(fss_test::wait_for([&]() {
        first = server_side_conn->getMsg();
        return first != nullptr;
    }));
    REQUIRE(first->getType() == message_type_identity);

    std::shared_ptr<fss_message> closed;
    REQUIRE(fss_test::wait_for([&]() {
        closed = server_side_conn->getMsg();
        return closed != nullptr;
    }));
    REQUIRE(closed->getType() == message_type_closed);

    REQUIRE(server_side_conn->getMsg() == nullptr);

    handoff.reset();
}

TEST_CASE("queue: 10k messages over loopback with no drops")
{
    handoff.reset();
    constexpr uint16_t port = 20504;
    auto listen = std::make_shared<large_queue_listen>(port, handoff.callback());
    REQUIRE(listen != nullptr);

    auto client = std::make_shared<fss_connection>();
    REQUIRE(client->connectTo("localhost", port));

    auto server_side_conn = handoff.wait();
    REQUIRE(server_side_conn != nullptr);

    constexpr int count = 10000;
    std::atomic<int> produced{0};
    std::thread producer([&]() {
        for (int i = 0; i < count; ++i)
        {
            auto msg = std::make_shared<fss_message_rtt_request>();
            if (!client->sendMsg(msg))
            {
                break;
            }
            produced.fetch_add(1);
        }
    });

    int received = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (received < count && std::chrono::steady_clock::now() < deadline)
    {
        auto msg = server_side_conn->getMsg();
        if (msg == nullptr)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (msg->getType() == message_type_closed)
        {
            break;
        }
        REQUIRE(msg->getType() == message_type_rtt_request);
        ++received;
    }
    producer.join();
    REQUIRE(produced.load() == count);
    REQUIRE(received == count);

    client = nullptr;
    server_side_conn = nullptr;
    handoff.reset();
}
