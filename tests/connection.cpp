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

#include <atomic>
#include <csignal>
#include <thread>

#include <pthread.h>
#include <sys/socket.h>
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

namespace {

/* Expose the protected fd-constructor and recvMsg so a test can drive the
 * receive path synchronously on its own thread (no recv thread started). */
class raw_recv_connection : public flight_safety_system::transport::fss_connection {
public:
    explicit raw_recv_connection(int t_fd) : fss_connection(t_fd) {}
    using fss_connection::recvMsg;
};

void noop_signal_handler(int /*signum*/) {}

} // namespace

TEST_CASE("recvMsg survives EINTR mid-message")
{
    /* Regression: a signal interrupting recv() (no SA_RESTART) used to be
     * treated as a connection failure, synthesising message_closed and
     * dropping a healthy peer.  Block in recvMsg with a partial header,
     * signal the receiving thread several times, then complete the message
     * and require it to arrive intact. */
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    struct sigaction sa = {};
    sa.sa_handler = noop_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* deliberately no SA_RESTART so recv() returns EINTR */
    struct sigaction old_sa = {};
    REQUIRE(sigaction(SIGUSR1, &sa, &old_sa) == 0);

    raw_recv_connection conn(fds[0]);

    auto send_msg = std::make_shared<flight_safety_system::transport::fss_message_identity>("eintrClient");
    auto bl = send_msg->getPacked();
    const char *data = bl->getData();
    size_t len = bl->getLength();
    REQUIRE(len > 1);

    /* First byte only: recvMsg blocks waiting for the rest of the header. */
    REQUIRE(::write(fds[1], data, 1) == 1);

    /* Catch2 assertions are not thread-safe: collect the worker's result in
     * an atomic and assert after join(), as the concurrency tests do. */
    std::atomic<ssize_t> rest_written{-1};
    pthread_t recv_thread_id = pthread_self();
    std::thread signaller([&]() -> void {
        /* Best-effort pause so the main thread reaches recv() before the
         * signals; correctness does not depend on it (an early signal is
         * swallowed by the no-op handler and the final write still
         * completes the message). */
        constexpr int signal_count = 5;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (int i = 0; i < signal_count; i++)
        {
            pthread_kill(recv_thread_id, SIGUSR1);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        rest_written.store(::write(fds[1], data + 1, len - 1));
    });

    auto msg = conn.recvMsg();
    signaller.join();

    REQUIRE(rest_written.load() == static_cast<ssize_t>(len - 1));
    REQUIRE(msg != nullptr);
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_identity);
    auto identity = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_identity>(msg);
    REQUIRE(identity != nullptr);
    REQUIRE(identity->getName() == "eintrClient");

    REQUIRE(sigaction(SIGUSR1, &old_sa, nullptr) == 0);
    ::close(fds[1]);
    /* fds[0] is owned and closed by conn's destructor. */
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
