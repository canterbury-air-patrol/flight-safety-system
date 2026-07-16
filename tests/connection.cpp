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

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include "fss-transport.hpp"
#include "test_helpers.hpp"
#include "transport.hpp"

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

/* The listener invokes its accept callback on its own worker thread; route the
 * accepted connection through a mutex-guarded handoff so the main thread reads
 * it with a happens-before edge. */
static fss_test::connection_handoff client_handoff;


TEST_CASE("Listen Socket")
{
    client_handoff.reset();
    constexpr int listen_port = 20202;
    auto listen = std::make_shared<flight_safety_system::transport::fss_listen>(listen_port, client_handoff.callback());
    REQUIRE(listen != nullptr);

    auto conn = std::make_shared<flight_safety_system::transport::fss_connection>();
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("localhost", listen_port));
    auto send_msg = std::make_shared<flight_safety_system::transport::fss_message_identity>("testClient");
    conn->sendMsg(send_msg);

    conn = nullptr;

    auto server_conn = client_handoff.wait();
    REQUIRE(server_conn != nullptr);

    std::shared_ptr<flight_safety_system::transport::fss_message> msg;
    REQUIRE(fss_test::wait_for([&]() {
        msg = server_conn->getMsg();
        return msg != nullptr;
    }));
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_identity);

    REQUIRE(fss_test::wait_for([&]() {
        msg = server_conn->getMsg();
        return msg != nullptr;
    }));
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_closed);

    msg = server_conn->getMsg();
    REQUIRE(msg == nullptr);

    client_handoff.reset();
}


class small_queue_listen : public flight_safety_system::transport::fss_listen {
public:
    static constexpr size_t queue_limit = 5;
    small_queue_listen(uint16_t t_port, flight_safety_system::transport::fss_connect_cb t_cb)
        : fss_listen(t_port, std::move(t_cb), defer_start_t{})
    {
        /* Start the accept thread only once this derived object is fully
         * constructed; the auto-starting base constructor would let the thread
         * virtual-dispatch into newConnection() before the vptr settles. */
        this->startListening();
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

    client_handoff.reset();
    auto listen = std::make_shared<small_queue_listen>(port, client_handoff.callback());
    REQUIRE(listen != nullptr);

    auto conn = std::make_shared<flight_safety_system::transport::fss_connection>();
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("localhost", port));

    auto server_conn = client_handoff.wait();
    REQUIRE(server_conn != nullptr);

    for (size_t i = 0; i < small_queue_listen::queue_limit + extra; ++i)
    {
        conn->sendMsg(std::make_shared<flight_safety_system::transport::fss_message_rtt_request>());
    }

    REQUIRE(fss_test::wait_for([&]() { return server_conn->getDroppedMessages() >= extra; }));
    REQUIRE(server_conn->getDroppedMessages() == extra);

    size_t count = 0;
    while (server_conn->getMsg() != nullptr)
    {
        ++count;
    }
    REQUIRE(count == small_queue_listen::queue_limit);

    conn = nullptr;
    server_conn = nullptr;
    client_handoff.reset();
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

/* Regression driver: a signal interrupting recv() (no SA_RESTART) used to be
 * treated as a connection failure, synthesising message_closed and dropping a
 * healthy peer.  Send the first prefix_len bytes of a framed identity message
 * so recvMsg blocks at the desired stage (1 byte = mid-header; the full
 * 2-byte length prefix and more = mid-body), signal the receiving thread
 * several times, then complete the message and require it to arrive intact. */
void run_eintr_recv_test(size_t prefix_len, const std::string &client_name)
{
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    fss_test::scoped_fd peer(fds[1]);
    raw_recv_connection conn(fds[0]); /* owns and closes fds[0] */

    fss_test::scoped_signal_handler sig_guard(SIGUSR1, noop_signal_handler);
    REQUIRE(sig_guard.ok());

    auto send_msg = std::make_shared<flight_safety_system::transport::fss_message_identity>(client_name);
    auto bl = send_msg->getPacked();
    const char *data = bl->getData();
    size_t len = bl->getLength();
    REQUIRE(len > prefix_len);

    REQUIRE(::write(peer.get(), data, prefix_len) == static_cast<ssize_t>(prefix_len));

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
        rest_written.store(::write(peer.get(), data + prefix_len, len - prefix_len));
    });

    auto msg = conn.recvMsg();
    signaller.join();

    REQUIRE(rest_written.load() == static_cast<ssize_t>(len - prefix_len));
    REQUIRE(msg != nullptr);
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_identity);
    auto identity = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_identity>(msg);
    REQUIRE(identity != nullptr);
    REQUIRE(identity->getName() == client_name);
}

} // namespace

TEST_CASE("recvMsg survives EINTR mid-header")
{
    /* One byte: recvMsg blocks reading the 2-byte length prefix. */
    run_eintr_recv_test(1, "eintrClient");
}

TEST_CASE("recvMsg survives EINTR mid-body")
{
    /* Full length prefix plus one byte: the header read completes and
     * recvMsg blocks in the body read loop. */
    run_eintr_recv_test(sizeof(uint16_t) + 1, "eintrBodyClient");
}

TEST_CASE("set_tcp_keepalive bounds unacked data with TCP_USER_TIMEOUT")
{
#ifdef TCP_USER_TIMEOUT
    /* Keepalives only cover idle connections; TCP_USER_TIMEOUT is what
     * bounds a blocking send() into a black-holed peer.  Regression-pin
     * that set_tcp_keepalive applies it. */
    int sock = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(sock >= 0);
    fss_test::scoped_fd guard(sock);
    unsigned int val = 0;
    socklen_t len = sizeof(val);

    SECTION("the default bound is 30s (pinned by literal on purpose)")
    {
        set_tcp_keepalive(sock, flight_safety_system::transport::default_tcp_user_timeout_ms);
        REQUIRE(::getsockopt(sock, IPPROTO_TCP, TCP_USER_TIMEOUT, &val, &len) == 0);
        REQUIRE(val == 30000);
    }

    SECTION("a caller-supplied bound is applied verbatim (todo/26)")
    {
        set_tcp_keepalive(sock, 5000);
        REQUIRE(::getsockopt(sock, IPPROTO_TCP, TCP_USER_TIMEOUT, &val, &len) == 0);
        REQUIRE(val == 5000);
    }
#endif
}

namespace {
/* Exposes the protected fd so a test can pin the socket option a connect
 * applied (todo/26); same pattern as fd_visible_listen below. */
class fd_visible_connection : public flight_safety_system::transport::fss_connection {
public:
    auto testGetFd() -> int { return this->getFd(); }
};
} // namespace

TEST_CASE("fss_connection: connectTo applies the connection's requested TCP_USER_TIMEOUT (todo/26)")
{
#ifdef TCP_USER_TIMEOUT
    /* A flight-safety client must be able to pick a send bound tighter than
     * the 30 s default (a wedged send worker recovers on its own clock, not
     * the server's liveness clock). Pin that setTcpUserTimeoutMs() before
     * connectTo() lands on the socket, and that a connection which never
     * asked keeps the default. */
    client_handoff.reset();
    const uint16_t listen_port = fss_test::pick_port();
    REQUIRE(listen_port != 0);
    auto listen = std::make_shared<flight_safety_system::transport::fss_listen>(listen_port, client_handoff.callback());

    unsigned int val = 0;
    socklen_t len = sizeof(val);

    SECTION("default: never asked, gets 30s")
    {
        auto conn = std::make_shared<fd_visible_connection>();
        REQUIRE(conn->connectTo("127.0.0.1", listen_port));
        REQUIRE(::getsockopt(conn->testGetFd(), IPPROTO_TCP, TCP_USER_TIMEOUT, &val, &len) == 0);
        REQUIRE(val == flight_safety_system::transport::default_tcp_user_timeout_ms);
    }

    SECTION("a tighter per-connection bound is honoured")
    {
        auto conn = std::make_shared<fd_visible_connection>();
        conn->setTcpUserTimeoutMs(5000);
        REQUIRE(conn->connectTo("127.0.0.1", listen_port));
        REQUIRE(::getsockopt(conn->testGetFd(), IPPROTO_TCP, TCP_USER_TIMEOUT, &val, &len) == 0);
        REQUIRE(val == 5000);
    }

    client_handoff.reset();
#endif
}

namespace {
/* Exposes the protected fd so a test can pin socket options on it (todo/42);
 * otherwise identical to the base fss_listen. */
class fd_visible_listen : public flight_safety_system::transport::fss_listen {
public:
    fd_visible_listen(uint16_t t_port, flight_safety_system::transport::fss_connect_cb t_cb)
        : fss_listen(t_port, std::move(t_cb), defer_start_t{})
    {
        /* Start the accept thread only once this derived object is fully
         * constructed; the auto-starting base constructor would let the thread
         * virtual-dispatch into newConnection() before the vptr settles
         * (the same TSan-flagged vptr-race hazard small_queue_listen above
         * already avoids this way). */
        this->startListening();
    }
    auto testGetFd() -> int { return this->getFd(); }
};
} // namespace

TEST_CASE("fss_listen: startListening requests dual-stack (IPV6_V6ONLY=0) (todo/42)")
{
    /* Regression-pin that startListening() explicitly requests dual-stack,
     * the same pin-the-socket-option pattern as TCP_USER_TIMEOUT above.
     * Without this, whether an IPv4 client can connect at all silently
     * depends on the host's net.ipv6.bindv6only sysctl. */
    client_handoff.reset();
    const auto port = fss_test::pick_port();
    REQUIRE(port != 0);
    fd_visible_listen listener(port, client_handoff.callback());

    int val = -1;
    socklen_t len = sizeof(val);
    REQUIRE(::getsockopt(listener.testGetFd(), IPPROTO_IPV6, IPV6_V6ONLY, &val, &len) == 0);
    REQUIRE(val == 0);

    /* Functional case: an IPv4 (well, IPv4-mapped, since connectTo always
     * resolves and connects over the family the address returns) client can
     * still complete a message round-trip. This already passes today on the
     * default sysctl -- the option pin above is what protects a hardened
     * host where it would otherwise silently refuse. */
    auto conn = std::make_shared<flight_safety_system::transport::fss_connection>();
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("127.0.0.1", port));
    conn->sendMsg(std::make_shared<flight_safety_system::transport::fss_message_identity>("v4Client"));

    auto server_conn = client_handoff.wait();
    REQUIRE(server_conn != nullptr);

    std::shared_ptr<flight_safety_system::transport::fss_message> msg;
    REQUIRE(fss_test::wait_for([&]() -> bool {
        msg = server_conn->getMsg();
        return msg != nullptr;
    }));
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_identity);

    client_handoff.reset();
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

    client_handoff.reset();
    auto listen = std::make_shared<flight_safety_system::transport::fss_listen>(listen_port, client_handoff.callback());
    REQUIRE(listen != nullptr);

    auto conn = std::make_shared<flight_safety_system::transport::fss_connection>();
    REQUIRE(conn != nullptr);
    REQUIRE(conn->connectTo("localhost", listen_port));

    conn->sendMsg(std::make_shared<flight_safety_system::transport::fss_message_identity>("testClient"));

    auto server_conn = client_handoff.wait();
    REQUIRE(server_conn != nullptr);

    auto cb = std::make_shared<fss_test::recording_message_cb>(server_conn);
    REQUIRE(cb->connected());
    REQUIRE(cb->getConnection() == server_conn);
    server_conn->setHandler(cb.get());

    REQUIRE(server_conn->getMsg() == nullptr);

    cb->sendMsg(std::make_shared<flight_safety_system::transport::fss_message_rtt_request>());

    REQUIRE(fss_test::wait_for([&]() { return cb->getFirstMsg() != nullptr; }));

    cb->disconnect();
    REQUIRE(!cb->connected());

    client_handoff.reset();
}
