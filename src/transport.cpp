#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <cstring>
#include <system_error>
#include <thread>
#include <vector>
#include "fss-transport.hpp"
#include "fss-log.hpp"

#include <iostream>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <cerrno>

#include "transport.hpp"

auto safe_close_fd(int fd, const char *context) -> int
{
    for (;;)
    {
        if (close(fd) == 0)
        {
            return 0;
        }
        if (errno == EINTR)
        {
            continue;
        }
        FSS_PERROR(context, "close failed on fd " + std::to_string(fd));
        return -1;
    }
}

auto safe_shutdown_fd(int fd, const char *context) -> int
{
    for (;;)
    {
        if (shutdown(fd, SHUT_RDWR) == 0)
        {
            return 0;
        }
        if (errno == ENOTCONN)
        {
            return 0;
        }
        if (errno == EINTR)
        {
            continue;
        }
        FSS_PERROR(context, "shutdown failed on fd " + std::to_string(fd));
        return -1;
    }
}

#ifdef DEBUG
/* Run inet_ntop on a sockaddr_storage object */
const char *inet_ntop_stor(struct sockaddr_storage *src, char *dst, size_t dstlen, uint16_t *port)
{
    switch (src->ss_family)
    {
        case AF_INET: {
            auto *sa_in = as_sockaddr_in(src);
            *port = ntohs(sa_in->sin_port);
            return inet_ntop(AF_INET, &sa_in->sin_addr, dst, dstlen);
        }
        case AF_INET6: {
            auto *sa_in6 = as_sockaddr_in6(src);
            *port = ntohs(sa_in6->sin6_port);
            return inet_ntop(AF_INET6, &sa_in6->sin6_addr, dst, dstlen);
        }
    }
    return nullptr;
}
#endif

flight_safety_system::transport::fss_connection::fss_connection() = default;

flight_safety_system::transport::fss_connection::fss_connection(
    int t_fd, size_t t_max_queue_size) // NOLINT(bugprone-easily-swappable-parameters)
    : fd(t_fd), max_queue_size(t_max_queue_size)
{
}

auto flight_safety_system::transport::fss_connection::create(int t_fd, size_t t_max_queue_size)
    -> std::shared_ptr<fss_connection>
{
    auto conn = std::shared_ptr<fss_connection>(new fss_connection(t_fd, t_max_queue_size));
    conn->startRecvThread(std::thread([conn]() -> void { conn->processMessages(); }));
    return conn;
}

auto flight_safety_system::transport::fss_connection::getDroppedMessages() -> uint64_t
{
    return this->dropped_messages.load();
}

auto flight_safety_system::transport::fss_connection::getNullMsgCount() -> uint64_t
{
    return this->consecutive_null_msgs.load();
}

void flight_safety_system::transport::fss_connection::disconnect()
{
    this->run.store(false);
    int orig_fd = this->fd.exchange(-1);
    if (orig_fd != -1)
    {
        safe_shutdown_fd(orig_fd, "transport/disconnect");
        safe_close_fd(orig_fd, "transport/disconnect");
    }
    if (this->recv_thread.joinable())
    {
        if (this->recv_thread.get_id() == std::this_thread::get_id())
        {
            /* Destructor called from within the recv thread itself (possible
             * when the lambda is the last shared_ptr owner).  Detach so the
             * thread can finish normally without trying to join itself. */
            this->recv_thread.detach();
            this->recv_thread = std::thread();
        }
        else
        {
            try
            {
                this->recv_thread.join();
            }
            catch (const std::system_error &e)
            {
                /* join() failed, so the thread is still joinable; leaving it
                 * would make ~std::thread call std::terminate. Detach to avoid
                 * that (leaking the thread) — we are already in a degenerate
                 * state where the thread could not be joined. */
                FSS_LOG_ERROR("transport", "recv_thread.join() failed, detaching: " << e.what());
                this->recv_thread.detach();
                this->recv_thread = std::thread();
            }
        }
    }
}

flight_safety_system::transport::fss_connection::~fss_connection()
{
    fss_connection::disconnect();
    while (!this->messages.empty())
    {
        auto msg = this->messages.front();
        this->messages.pop();
    }
}

auto flight_safety_system::transport::fss_connection::getMessageId() -> uint64_t
{
    return ++this->last_msg_id;
}

void flight_safety_system::transport::fss_connection::processMessages()
{
    this->run.store(true);
    flight_safety_system::exception_guard handler_guard("transport", "processMessage");
    while (this->run.load())
    {
        auto msg = this->recvMsg();
        if (msg == nullptr)
        {
            /* Undecodable frame (zero/short declared length, unknown type,
             * or decode mismatch). A peer streaming garbage drives this
             * loop at line rate, so throttle the log to the first frame and
             * every null_msg_log_interval-th after — otherwise a single bad
             * peer floods the log unboundedly. */
            uint64_t nulls = ++this->consecutive_null_msgs;
            if (nulls == 1 || (nulls % null_msg_log_interval) == 0)
            {
                FSS_LOG_WARN("transport", "Got a null msg (" << nulls << " consecutive undecodable frames)");
            }
            continue;
        }
        this->consecutive_null_msgs.store(0);
        if (msg->getType() == message_type_closed)
        {
            FSS_LOG_INFO("transport", "Remote closed the connection");
            this->run.store(false);
            {
                std::scoped_lock lock_holder(this->msg_lock);
                if (this->handler != nullptr)
                {
                    handler_guard.run([&]() -> void { this->handler->processMessage(msg); });
                }
                else
                {
                    this->messages.push(msg);
                }
            }
            break;
        }
        {
            std::scoped_lock lock_holder(this->msg_lock);
            if (this->handler != nullptr)
            {
                handler_guard.run([&]() -> void { this->handler->processMessage(msg); });
            }
            else
            {
                if (this->max_queue_size != 0 && this->messages.size() >= this->max_queue_size)
                {
                    this->messages.pop();
                    uint64_t dropped = ++this->dropped_messages;
                    if (dropped == 1 || dropped % 100 == 0)
                    {
                        FSS_LOG_WARN("transport",
                                     "Message queue full, dropping oldest message. Total dropped: " << dropped);
                    }
                }
                this->messages.push(msg);
            }
        }
    }
}

auto flight_safety_system::transport::fss_connection::getMsg()
    -> std::shared_ptr<flight_safety_system::transport::fss_message>
{
    std::scoped_lock lock_holder(this->msg_lock);
    if (this->handler == nullptr)
    {
        if (!this->messages.empty())
        {
            auto msg = this->messages.front();
            if (msg != nullptr)
            {
                this->messages.pop();
            }
            return msg;
        }
    }
    return nullptr;
}

auto flight_safety_system::transport::fss_connection::connectTo(const std::string &address, uint16_t port) -> bool
{
    struct sockaddr_storage remote = {};
    if (!convert_str_to_sa(address, port, &remote))
    {
        FSS_LOG_ERROR("transport", "Failed to convert '" << address << "' to a usable address");
        return false;
    }

#ifdef DEBUG
    char addr_str[INET6_ADDRSTRLEN];
    uint16_t client_port;
    inet_ntop_stor(&remote, addr_str, INET6_ADDRSTRLEN, &client_port);
    std::cout << "Trying to connect to " << address << " (" << addr_str << "):" << port << std::endl;
#endif

    if (this->fd.load() == -1)
    {
        int new_fd = socket(remote.ss_family == AF_INET ? PF_INET : PF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (new_fd < 0)
        {
            FSS_PERROR("transport", "Failed to create socket");
            return false;
        }
        this->fd.store(new_fd);
    }

    int current_fd = this->fd.load();
    // Limit the total number of SYN's that are sent
    int synRetries = 2;
    if (setsockopt(current_fd, IPPROTO_TCP, TCP_SYNCNT, &synRetries, sizeof(synRetries)) < 0)
    {
        FSS_PERROR("transport", "setsockopt TCP_SYNCNT failed, using kernel default");
    }

    if (connect(current_fd, as_sockaddr(&remote),
                remote.ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)) < 0)
    {
        FSS_PERROR("transport", "Failed to connect to " + address);
        safe_close_fd(this->fd.exchange(-1), "transport/connect");
        return false;
    }

    set_tcp_keepalive(current_fd);

    this->startRecvThread(std::thread([this]() -> void { this->processMessages(); }));

    return true;
}

auto flight_safety_system::transport::fss_connection::sendMsg(const std::shared_ptr<fss_message> &msg) -> bool
{
    std::scoped_lock lock_holder(this->send_lock);
    msg->setId(this->getMessageId());
    auto bl = msg->getPacked();
#ifdef DEBUG
    std::cout << "Sending message (len=" << bl->getLength() << ") to " << this->fd << std::endl;
#endif
    if (!bl->isValid())
    {
        return false;
    }
    bool ret = this->sendMsg(bl);
    return ret;
}

#ifdef DEBUG
static void print_bl(std::shared_ptr<flight_safety_system::transport::buf_len> bl)
{
    const auto *data = static_cast<const unsigned char *>(static_cast<const void *>(bl->getData()));
    size_t len = bl->getLength();
    for (size_t o = 0; o < len; o++)
    {
        printf("0x%04zx: 0x%02x '%1c'\n", o, data[o], data[o]);
    }
    printf("\n");
}
#endif

auto flight_safety_system::transport::fss_connection::sendMsg(const std::shared_ptr<buf_len> &bl) -> bool
{
    int current_fd = this->fd.load();
    if (current_fd == -1)
    {
        return false;
    }
    size_t sent = 0;
    size_t to_send = bl->getLength();
    const char *data = bl->getData();
#ifdef DEBUG
    print_bl(bl);
#endif
    while (sent < to_send)
    {
        /* Re-load fd every iteration: a concurrent disconnect() does
         * fd.exchange(-1) then closes the descriptor.  If that races with
         * a multi-iteration send we must stop rather than write into a
         * stale (possibly reused) descriptor. */
        current_fd = this->fd.load();
        if (current_fd == -1)
        {
            return false;
        }
        ssize_t transfered = send(current_fd, &data[sent], to_send - sent, 0);
        if (transfered < 0)
        {
            /* A signal can interrupt send() before any bytes are written;
             * that is not a connection failure, so retry rather than dropping
             * the peer. SIGPIPE is ignored process-wide, so a genuinely broken
             * connection surfaces here as EPIPE (or similar) and falls through
             * to return false.
             *
             * These sockets are always blocking — O_NONBLOCK is never set —
             * so send() does not return EAGAIN/EWOULDBLOCK here; it blocks
             * until buffer space is available. If a socket is ever made
             * non-blocking, this loop must also retry (with backoff) on
             * EAGAIN/EWOULDBLOCK to avoid dropping the connection. */
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        sent += transfered;
    }
    return true;
}

void flight_safety_system::transport::fss_connection::setHandler(fss_message_cb *cb)
{
    std::scoped_lock lock_holder(this->msg_lock);
    this->handler = cb;
    if (this->handler != nullptr)
    {
        while (!this->messages.empty())
        {
            auto msg = this->messages.front();
            this->messages.pop();
            cb->processMessage(msg);
        }
    }
}

auto flight_safety_system::transport::fss_connection::recvBytes(void *t_bytes, size_t t_max_bytes) -> ssize_t
{
    for (;;)
    {
        int current_fd = this->fd.load();
        if (current_fd < 0)
        {
            return -2;
        }
        ssize_t received = recv(current_fd, t_bytes, t_max_bytes, 0);
        if (received == -1 && errno == EINTR)
        {
            /* A signal interrupted recv() before any data arrived; that is
             * not a connection failure, so retry rather than reporting an
             * error to the caller (same rationale as the EINTR retry in
             * sendMsg).  The fd is re-loaded every iteration so a concurrent
             * disconnect() (fd.exchange(-1) then close) still terminates the
             * loop via the -2 path.  This keeps the contract aligned with
             * the transport_ssl override, which likewise retries
             * GNUTLS_E_INTERRUPTED internally: recvBytes never reports an
             * interrupted call to recvMsg. */
            continue;
        }
        return received;
    }
}

auto flight_safety_system::transport::fss_connection::getFd() -> int
{
    return this->fd.load();
}

void flight_safety_system::transport::fss_connection::setFd(int new_fd)
{
    this->fd.store(new_fd);
}

void flight_safety_system::transport::fss_connection::startRecvThread(std::thread t_recv_thread)
{
    this->recv_thread = std::move(t_recv_thread);
}


auto flight_safety_system::transport::fss_connection::recvMsg()
    -> std::shared_ptr<flight_safety_system::transport::fss_message>
{
    std::shared_ptr<flight_safety_system::transport::fss_message> msg = nullptr;
    std::array<std::uint8_t, sizeof(uint16_t)> header{};
    ssize_t received = 0;
    while (received < static_cast<ssize_t>(sizeof(uint16_t)))
    {
        ssize_t this_time = this->recvBytes(&header[received], sizeof(uint16_t) - received);
        if ((this_time == -2) || (this_time < 0 && errno == EBADF) || this_time == 0)
        {
            /* Connection was closed */
            return std::make_shared<flight_safety_system::transport::fss_message_closed>();
        }
        if (this_time < 0)
        {
            FSS_PERROR("transport", "Failed to get header");
            return std::make_shared<flight_safety_system::transport::fss_message_closed>();
        }
        received += this_time;
    }
    uint16_t data_length_n;
    memcpy(&data_length_n, header.data(), sizeof(uint16_t));
    uint16_t data_length = ntohs(data_length_n);
    if (data_length > FSS_MAX_MESSAGE_BYTES)
    {
        FSS_LOG_ERROR("transport", "Message too large (" << data_length << " bytes), closing connection");
        this->disconnect();
        return std::make_shared<flight_safety_system::transport::fss_message_closed>();
    }
    auto total_length = static_cast<ssize_t>(data_length);
    if (data_length % sizeof(uint64_t) != 0)
    {
        total_length +=
            static_cast<ssize_t>(sizeof(uint64_t)) - (total_length % static_cast<ssize_t>(sizeof(uint64_t)));
    }
    if (total_length < static_cast<ssize_t>(sizeof(uint16_t)))
    {
        return msg;
    }
    /* Get the full message (prefixed with the header for decode) */
    std::vector<std::uint8_t> data(total_length);
    memcpy(data.data(), header.data(), sizeof(uint16_t));
    received = sizeof(uint16_t);
    while (received != total_length)
    {
        ssize_t this_time = this->recvBytes(&data[received], total_length - received);
        if (this_time < 0)
        {
            FSS_PERROR("transport", "Error receiving data");
            break;
        }
        else if (this_time == 0)
        {
            /* Connection was closed */
            break;
        }
        received += this_time;
    }
    if (received == total_length)
    {
        auto bl = std::make_shared<buf_len>(data.data(), data_length);
#ifdef DEBUG
        printf("Message reads: \n");
        print_bl(bl);
#endif
        msg = flight_safety_system::transport::fss_message::decode(bl);
    }
    else
    {
        FSS_PERROR("transport", "Failed to get all the data");
    }
    return msg;
}

auto flight_safety_system::transport::fss_connection::getClientNames() -> std::list<std::string>
{
    std::list<std::string> ret;
    return ret;
}

flight_safety_system::transport::fss_listen::fss_listen(uint16_t t_port, fss_connect_cb t_cb)
    : fss_connection(), port(t_port), cb(std::move(t_cb))
{
    this->startListening();
}

flight_safety_system::transport::fss_listen::fss_listen(uint16_t t_port, fss_connect_cb t_cb, defer_start_t /*tag*/)
    : fss_connection(), port(t_port), cb(std::move(t_cb))
{
    /* Derived constructor calls startListening() when it is done. */
}

flight_safety_system::transport::fss_listen::~fss_listen()
{
    /* Qualified (non-virtual) call: invoke this class's own disconnect during
     * destruction. A plain this->disconnect() is a virtual call in a
     * destructor — flagged by clang-analyzer and pointless here since the
     * derived part is already gone. */
    flight_safety_system::transport::fss_listen::disconnect();
}

static void listen_thread(flight_safety_system::transport::fss_listen *listen)
{
    listen->processMessages();
}

void flight_safety_system::transport::fss_listen::processMessages()
{
    while (this->getFd() >= 0)
    {
        struct sockaddr_storage sa = {};
        socklen_t sa_len = sizeof(sockaddr_storage);
        int newfd = accept(this->getFd(), as_sockaddr(&sa), &sa_len);
        if (newfd < 0)
        {
            if (errno == EBADF)
            {
                return;
            }
            FSS_PERROR("transport", "Failed to accept");
            /* On resource exhaustion (out of file descriptors or memory) the
             * pending connection is not consumed, so accept() would fail again
             * immediately and spin the CPU at 100% while flooding the log.
             * Back off briefly to give the system a chance to recover; other
             * errno values (e.g. ECONNABORTED, EINTR) are transient and safe
             * to retry without delay. */
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM)
            {
                constexpr auto accept_backoff = std::chrono::milliseconds(50);
                std::this_thread::sleep_for(accept_backoff);
            }
            continue;
        }
#ifdef DEBUG
        char addr_str[INET6_ADDRSTRLEN];
        uint16_t client_port;
        inet_ntop_stor(&sa, addr_str, INET6_ADDRSTRLEN, &client_port);
        std::cout << "New client from " << addr_str << ":" << client_port << " as " << newfd << std::endl;
#endif
        set_tcp_keepalive(newfd);
        if (this->cb == nullptr)
        {
            /* Thanks for your call, unfortunately we don't know how to deal with it */
            safe_close_fd(newfd, "transport/accept");
            continue;
        }
        /* Reserve a setup slot, or shed load if we are at the bound / shutting
         * down. Closing the fd here lets the peer observe the rejection
         * immediately rather than queueing unboundedly. */
        {
            std::scoped_lock setup_holder(this->setup_lock);
            if (!this->accepting_setups || this->active_setups >= this->max_concurrent_setups)
            {
                uint64_t rejected = ++this->rejected_setups;
                if (rejected == 1 || (rejected % 100) == 0)
                {
                    FSS_LOG_WARN("transport", "Connection setup slots exhausted ("
                                                  << this->active_setups << "/" << this->max_concurrent_setups
                                                  << "), dropping connection. Total dropped: " << rejected);
                }
                safe_close_fd(newfd, "transport/accept");
                continue;
            }
            ++this->active_setups;
        }
        /* The slot is reserved; hand the fd to a worker. If the worker thread
         * cannot be created it never runs to release the slot, so roll the
         * reservation back here (and wake any waiting drain) and drop the fd —
         * otherwise disconnect() would block forever on active_setups != 0. */
        try
        {
            this->startSetupWorker(newfd);
        }
        catch (const std::system_error &e)
        {
            FSS_LOG_ERROR("transport", "Failed to start connection setup worker: " << e.what());
            this->releaseSetupSlot();
            safe_close_fd(newfd, "transport/accept");
        }
    }
}

void flight_safety_system::transport::fss_listen::startSetupWorker(int t_newfd)
{
    /* Run newConnection() (the blocking TLS handshake for the SSL listener) and
     * the connect callback off the accept thread, so a slow or silent peer
     * cannot block accept() for other clients. The worker is detached;
     * disconnect() drains in-flight workers before this object dies, so the raw
     * `this` capture stays valid for the worker's lifetime. */
    std::thread([this, t_newfd]() -> void {
        flight_safety_system::exception_guard setup_guard("transport", "new connection setup");
        setup_guard.run([&]() -> void {
            auto conn = this->newConnection(t_newfd);
            if (conn != nullptr && this->cb != nullptr)
            {
                this->cb(conn);
            }
        });
        this->releaseSetupSlot();
    }).detach();
}

void flight_safety_system::transport::fss_listen::releaseSetupSlot()
{
    /* notify_all() runs while the lock is held so disconnect() cannot wake
     * (re-lock), see active_setups==0, and destroy setup_cv before this notify
     * completes. The worker must touch no other `this` state afterwards. */
    std::scoped_lock setup_holder(this->setup_lock);
    --this->active_setups;
    this->setup_cv.notify_all();
}

void flight_safety_system::transport::fss_listen::disconnect()
{
    /* Stop the accept loop and join the accept thread first, so no new setup
     * workers can be spawned, then drain the ones already in flight. They read
     * members of derived listeners (e.g. the SSL cert/key paths), so this must
     * complete before those members are destroyed. */
    flight_safety_system::transport::fss_connection::disconnect();
    std::unique_lock setup_holder(this->setup_lock);
    this->accepting_setups = false;
    this->setup_cv.wait(setup_holder, [this]() -> bool { return this->active_setups == 0; });
}

void flight_safety_system::transport::fss_listen::setMaxConcurrentSetups(size_t t_max)
{
    if (t_max == 0)
    {
        /* A bound of 0 would refuse every connection (active_setups >= 0 is
         * always true). That is never intended, so ignore it and keep the
         * current bound rather than silently bricking the listener. */
        FSS_LOG_WARN("transport", "setMaxConcurrentSetups(0) ignored: a zero bound would refuse all connections");
        return;
    }
    std::scoped_lock setup_holder(this->setup_lock);
    this->max_concurrent_setups = t_max;
}

auto flight_safety_system::transport::fss_listen::getActiveSetupCount() -> size_t
{
    std::scoped_lock setup_holder(this->setup_lock);
    return this->active_setups;
}

auto flight_safety_system::transport::fss_listen::getRejectedSetupCount() -> uint64_t
{
    return this->rejected_setups.load();
}

auto flight_safety_system::transport::fss_listen::newConnection(int t_newfd)
    -> std::shared_ptr<flight_safety_system::transport::fss_connection>
{
    return flight_safety_system::transport::fss_connection::create(t_newfd);
}

auto flight_safety_system::transport::fss_listen::startListening() -> bool
{
    /* open the socket */
    if (this->getFd() < 0)
    {
        this->setFd(socket(PF_INET6, SOCK_STREAM, IPPROTO_TCP));
        if (this->getFd() < 0)
        {
            FSS_PERROR("transport", "Failed to open socket");
            return false;
        }
    }
    int reuse = 1;
    if (setsockopt(this->getFd(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        FSS_PERROR("transport", "setsockopt SO_REUSEADDR failed on port " + std::to_string(this->port));
    }
    struct sockaddr_in6 bind_addr = {};
    bind_addr.sin6_family = AF_INET6;
    bind_addr.sin6_port = htons(this->port);
    if (bind(this->getFd(), as_sockaddr(&bind_addr), sizeof(bind_addr)) < 0)
    {
        FSS_PERROR("transport", "Failed to bind socket");
        safe_close_fd(this->getFd(), "transport/listen");
        this->setFd(-1);
        return false;
    }
    if (listen(this->getFd(), this->max_pending_connections) < 0)
    {
        FSS_PERROR("transport", "Failed to listen on socket");
        safe_close_fd(this->getFd(), "transport/listen");
        this->setFd(-1);
        return false;
    }
    this->startRecvThread(std::thread(listen_thread, this));
    return true;
}

flight_safety_system::transport::fss_message_cb::~fss_message_cb()
{
    // No lock: destructor runs only when no other thread holds a reference to
    // this object, so conn cannot be concurrently read or written here.
    if (this->conn != nullptr)
    {
        this->conn->setHandler(nullptr);
        this->conn.reset();
    }
}
