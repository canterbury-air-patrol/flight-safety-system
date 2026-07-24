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

/* Atomically take whatever fd is parked in `slot` (if any) and close it.
 * Shared by every todo/52 deferred-close consumer (shutdownSocket()'s
 * stale-pending rollover, disconnect()'s quiesced-thread close, and the
 * destructor's backstop) so the exchange-then-close pattern lives in one
 * place. */
static void close_pending_fd(std::atomic<int> &slot, const char *context)
{
    int fd = slot.exchange(-1);
    if (fd != -1)
    {
        safe_close_fd(fd, context);
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

void flight_safety_system::transport::fss_connection::shutdownSocket()
{
    int orig_fd = this->fd.exchange(-1);
    if (orig_fd == -1)
    {
        return;
    }
    /* shutdown() operates on the still-open socket, so it is what actually
     * unblocks a peer thread stuck in recv()/send(); the I/O loops then bail
     * on the -1 they re-load. Publish the fd for the deferred close only
     * after the shutdown, so whoever performs the close can never observe it
     * un-shut-down. */
    safe_shutdown_fd(orig_fd, "transport/shutdown");
    int prev = this->pending_close_fd.exchange(orig_fd);
    if (prev != -1)
    {
        /* Only possible when this object was reconnected after a deferred
         * close was left pending; every thread of that earlier session is
         * past its last syscall on it, so release it rather than leak it.
         * prev is already taken out of pending_close_fd above (the exchange
         * that just stored orig_fd), so close it directly rather than
         * through close_pending_fd (which would re-exchange a slot that no
         * longer holds it). */
        safe_close_fd(prev, "transport/shutdown");
    }
}

void flight_safety_system::transport::fss_connection::disconnect()
{
    this->run.store(false);
    /* Shut down first, close later (todo/52): close() frees the descriptor
     * NUMBER for reuse, so it must wait until no thread that could still pass
     * the old number to a syscall is unjoined — a setup worker accepting a
     * new client can be handed the same number back, turning a late
     * recv()/send() into I/O on an unrelated session. */
    this->shutdownSocket();
    bool recv_thread_quiesced = true;
    if (this->recv_thread.joinable())
    {
        if (this->recv_thread.get_id() == std::this_thread::get_id())
        {
            /* disconnect() called from within the recv thread itself (garbage
             * threshold, oversized frame, or a destructor running there when
             * the lambda held the last shared_ptr).  Detach so the thread can
             * finish normally without trying to join itself. This thread
             * issues no further I/O, but a caller-owned sender thread (the
             * server-side fss_client outbound worker) may still be about to;
             * leave the close pending for the owner's own disconnect() — which
             * runs after it joined its sender — or the destructor. */
            this->recv_thread.detach();
            this->recv_thread = std::thread();
            recv_thread_quiesced = false;
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
                 * state where the thread could not be joined. It may still be
                 * running, so the fd must stay reserved as well: leave the
                 * close pending for the destructor. */
                FSS_LOG_ERROR("transport", "recv_thread.join() failed, detaching: " << e.what());
                this->recv_thread.detach();
                this->recv_thread = std::thread();
                recv_thread_quiesced = false;
            }
        }
    }
    if (recv_thread_quiesced)
    {
        close_pending_fd(this->pending_close_fd, "transport/disconnect");
    }
    /* Callbacks now run outside msg_lock (todo/25), so joining the recv thread
     * is no longer the only thing that can prove one is not still running —
     * and in the two detach branches above it never proved it at all. Wait for
     * delivery-idle so that once disconnect() returns, no processMessage() is
     * in flight. That is what keeps the fss_client teardown path safe: it
     * clears its connection pointer before ~fss_message_cb runs, so it never
     * reaches setHandler(nullptr) and this is its only barrier. A disconnect()
     * issued from inside a handler is exempt, mirroring the self-detach branch. */
    {
        std::unique_lock lock_holder(this->msg_lock);
        this->waitForDeliveryIdle(lock_holder);
    }
}

flight_safety_system::transport::fss_connection::~fss_connection()
{
    fss_connection::disconnect();
    /* Backstop for the deferred-close paths disconnect() cannot finish itself
     * (recv-thread self-disconnect, failed join): once the last owner is
     * destroying this object no thread can still be about to use the fd, so
     * release the descriptor number now. */
    close_pending_fd(this->pending_close_fd, "transport/destructor");
    /* Under msg_lock: disconnect() above detaches rather than joins on two
     * paths, so a recv thread can still be inside processMessages() touching
     * the queue. It can no longer be inside a callback (disconnect() waits for
     * delivery-idle), but it may still be queueing. */
    const std::scoped_lock lock_holder(this->msg_lock);
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

void flight_safety_system::transport::fss_connection::waitForDeliveryIdle(std::unique_lock<std::mutex> &t_lock)
{
    while (this->delivery_depth != 0 && this->delivering_thread != std::this_thread::get_id())
    {
        this->delivery_cv.wait(t_lock);
    }
}

void flight_safety_system::transport::fss_connection::deliverUnlocked(
    std::unique_lock<std::mutex> &t_lock, const std::shared_ptr<flight_safety_system::transport::fss_message> &msg,
    flight_safety_system::exception_guard *t_guard)
{
    /* Copy the handler out before releasing the lock: setHandler() cannot
     * change it again until delivery_depth returns to 0, so the local stays
     * valid for the whole call (see the `handler` member's lifetime note). */
    auto *cb = this->handler;
    if (this->delivery_depth++ == 0)
    {
        this->delivering_thread = std::this_thread::get_id();
    }
    t_lock.unlock();
    try
    {
        if (t_guard != nullptr)
        {
            t_guard->run([&]() -> void { cb->processMessage(msg); });
        }
        else
        {
            cb->processMessage(msg);
        }
    }
    catch (...)
    {
        /* Only reachable with t_guard == nullptr (setHandler's flush, which
         * propagates handler exceptions as it always has). Unwind the delivery
         * state before letting the exception out, or the connection would look
         * permanently busy and every later delivery would block forever. */
        t_lock.lock();
        this->endDeliveryLocked();
        throw;
    }
    t_lock.lock();
    this->endDeliveryLocked();
}

void flight_safety_system::transport::fss_connection::endDeliveryLocked()
{
    if (--this->delivery_depth == 0)
    {
        this->delivering_thread = std::thread::id();
        this->delivery_cv.notify_all();
    }
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
            if (nulls < null_msg_disconnect_threshold)
            {
                if (nulls == 1 || (nulls % null_msg_log_interval) == 0)
                {
                    FSS_LOG_WARN("transport", "Got a null msg (" << nulls << " consecutive undecodable frames)");
                }
                continue;
            }
            /* Threshold of consecutive garbage frames reached: close the
             * session loudly, mirroring the oversized-frame defence in
             * recvMsg(). Synthesise a closed message and fall through so the
             * normal close-delivery path below notifies the handler. */
            FSS_LOG_ERROR("transport", "peer sent " << nulls << " consecutive undecodable frames, closing");
            this->disconnect();
            msg = std::make_shared<flight_safety_system::transport::fss_message_closed>();
        }
        this->consecutive_null_msgs.store(0);
        if (msg->getType() == message_type_closed)
        {
            FSS_LOG_INFO("transport", "Remote closed the connection");
            this->run.store(false);
            {
                std::unique_lock lock_holder(this->msg_lock);
                /* Wait out any delivery already in flight (a setHandler()
                 * backlog flush on another thread), then re-read the handler:
                 * that flush may have detached it while we waited. */
                this->waitForDeliveryIdle(lock_holder);
                if (this->handler != nullptr)
                {
                    this->deliverUnlocked(lock_holder, msg, &handler_guard);
                }
                else
                {
                    this->messages.push(msg);
                }
            }
            break;
        }
        {
            std::unique_lock lock_holder(this->msg_lock);
            this->waitForDeliveryIdle(lock_holder);
            if (this->handler != nullptr)
            {
                this->deliverUnlocked(lock_holder, msg, &handler_guard);
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

    set_tcp_keepalive(current_fd, this->getTcpUserTimeoutMs());

    this->startRecvThread(std::thread([this]() -> void { this->processMessages(); }));

    return true;
}

auto flight_safety_system::transport::fss_connection::sendMsg(const std::shared_ptr<fss_message> &msg) -> bool
{
    std::scoped_lock lock_holder(this->send_lock);
    uint64_t assigned_id = this->getMessageId();
    msg->setId(assigned_id);
    auto bl = msg->getPacked();
#ifdef DEBUG
    std::cout << "Sending message (len=" << bl->getLength() << ") to " << this->fd << std::endl;
#endif
    if (!bl->isValid())
    {
        /* todo/38: the message never went out (most commonly because it
         * exceeds the 16-bit length field and updateSize() invalidated the
         * buffer), so the id just assigned above must not leave a gap — the
         * peer's v2 sequence check treats any gap as out-of-order (todo/39).
         * Roll back under send_lock (still held here), the same lock
         * getMessageId() incremented it under, so no concurrent sender can
         * observe or reuse the rolled-back value. */
        --this->last_msg_id;
        return false;
    }
    bool ret = this->sendMsg(bl);
    if (msg->getType() == flight_safety_system::transport::message_type_smm_settings)
    {
        /* todo/43: scrub the packed credential bytes once the send attempt
         * is done (success or failure) rather than leaving them resident
         * until bl's refcount drops. */
        bl->wipeSecure();
    }
    return ret;
}

auto flight_safety_system::transport::fss_connection::sendPacked(const std::shared_ptr<const buf_len> &packed) -> bool
{
    if (packed == nullptr || !packed->isValid())
    {
        return false;
    }
    std::scoped_lock lock_holder(this->send_lock);
    /* Enforce the no-credentials precondition at runtime, not just by convention:
     * this path deliberately skips the message_type_smm_settings wipeSecure scrub
     * sendMsg() does (todo/43), and a shared broadcast frame must never carry
     * credentials in the first place. Refuse (loudly — a routing bug) before an id
     * is consumed, so a misrouted settings frame is dropped rather than broadcast
     * in the clear. */
    if (fss_message::peekType(*packed) == flight_safety_system::transport::message_type_smm_settings)
    {
        FSS_LOG_ERROR("transport", "sendPacked refused a message_type_smm_settings frame; credentials must not be "
                                   "broadcast — dropping");
        return false;
    }
    uint64_t assigned_id = this->getMessageId();
    /* Clone the bytes, not the message (todo/55): the shared `packed` frame is
     * read-only and identical for every recipient — the only per-connection
     * difference is the id — so copy it once and stamp the id straight in at the
     * fixed header offset (fss_message owns that layout), bypassing decode and
     * re-pack entirely. */
    auto bl = std::make_shared<buf_len>(*packed);
    if (!fss_message::stampId(*bl, assigned_id))
    {
        /* Too short to hold a header — the frame the broadcaster validated should
         * never be, but if it is, roll the id back (still under send_lock, as
         * getMessageId() incremented it) so the peer's v2 sequence check sees no
         * gap (todo/38, todo/39), exactly as sendMsg(fss_message) does. */
        --this->last_msg_id;
        return false;
    }
    return this->sendMsg(bl);
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
        /* Re-load fd every iteration so a multi-iteration send stops promptly
         * when a concurrent shutdownSocket()/disconnect() retires the fd.
         * This alone does not prevent send() on a stale value (the load-vs-
         * syscall window is unavoidable); what does is the deferred close
         * (todo/52): the descriptor number is only released once the threads
         * that could still send on it are joined, so until then a late send()
         * hits the shut-down-but-open socket and fails with EPIPE. */
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
    std::unique_lock lock_holder(this->msg_lock);
    /* Nothing may be in flight into the outgoing handler when we swap it out:
     * this wait is what makes setHandler(nullptr) — in particular the one in
     * ~fss_message_cb — a barrier proving the handler outlives every call the
     * transport makes into it (todo/25). A handler that calls setHandler()
     * from inside its own processMessage() is exempt and does not self-block. */
    this->waitForDeliveryIdle(lock_holder);
    this->handler = cb;
    /* Re-read this->handler each iteration rather than trusting cb: a flushed
     * callback may re-enter setHandler(nullptr) to detach mid-flush, and the
     * rest of the backlog must then stay queued rather than be delivered to a
     * handler that has just asked to stop hearing from us. */
    while (this->handler != nullptr && !this->messages.empty())
    {
        auto msg = this->messages.front();
        this->messages.pop();
        this->deliverUnlocked(lock_holder, msg, nullptr);
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
        if (msg != nullptr && msg->getType() == flight_safety_system::transport::message_type_smm_settings)
        {
            /* todo/43: unpackData() has already copied the credentials into
             * secure_string members; scrub both transient copies of the
             * plaintext frame — this vector and bl's internal buffer —
             * rather than leaving them resident until deallocation. */
            explicit_bzero(data.data(), data.size());
            bl->wipeSecure();
        }
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
        /* Server-accepted sockets always get the default bound; only a
         * client's outbound connection can request a tighter one (todo/26). */
        set_tcp_keepalive(newfd, flight_safety_system::transport::default_tcp_user_timeout_ms);
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
            if (conn == nullptr)
            {
                return;
            }
            /* The callback only takes ownership by returning true. On every
             * other outcome — false (declined), no callback, or a throw —
             * the connection must be actively retired: its recv thread holds
             * a shared_ptr to it (the create() lambda capture), so merely
             * dropping ours here would leave it alive forever — fd open,
             * thread running, messages queueing — with nobody able to reach
             * it again. Same leaked-zombie mechanism the client reconnect
             * path had (todo/65). */
            bool adopted = false;
            try
            {
                adopted = this->cb != nullptr && this->cb(conn);
            }
            catch (...)
            {
                conn->disconnect();
                throw; // setup_guard logs it
            }
            if (!adopted)
            {
                conn->disconnect();
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
    /* Explicitly request dual-stack: this socket must accept both IPv6 and
     * IPv4-mapped connections, matching connectTo()'s ability to reach either
     * family. Without this, whether IPv4 clients can connect at all depends
     * on the host's net.ipv6.bindv6only sysctl -- 0 (the Linux default)
     * happens to give dual-stack, but 1 (some hardening baselines, and the
     * default on some BSDs) silently refuses every IPv4 client with nothing
     * in the log to say why. A failed setsockopt here just means the sysctl
     * default applies, same as before this call existed. */
    int v6only = 0;
    if (setsockopt(this->getFd(), IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0)
    {
        FSS_PERROR("transport", "setsockopt IPV6_V6ONLY failed on port " + std::to_string(this->port));
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
        /* setHandler() propagates exceptions thrown by a handler it flushes the
         * backlog into, and a destructor must not. Detaching (cb == nullptr)
         * never flushes anything, so this cannot fire — but the compiler and
         * cppcheck only see a potentially-throwing call in a noexcept function,
         * and a stray throw here would be std::terminate rather than a leak. */
        try
        {
            this->conn->setHandler(nullptr);
        }
        catch (const std::exception &e)
        {
            FSS_LOG_ERROR("transport", "Exception detaching handler in ~fss_message_cb: " << e.what());
        }
        catch (...)
        {
            FSS_LOG_ERROR("transport", "Unknown exception detaching handler in ~fss_message_cb");
        }
        this->conn.reset();
    }
}
