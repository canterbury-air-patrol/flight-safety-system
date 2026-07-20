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
#include <cerrno>
#include <chrono>
#include <memory>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "fss-transport.hpp"
#include "test_helpers.hpp"

using flight_safety_system::transport::fss_connection;
using flight_safety_system::transport::fss_message;
using flight_safety_system::transport::message_type_closed;
using flight_safety_system::transport::FSS_MAX_MESSAGE_BYTES;

namespace {

/* Pins the todo/52 close ordering: fd-number reuse is invisible to TSan and
 * the e2e suite (it is not a data race), so these tests assert the ordering
 * that prevents it instead — the descriptor must still be OPEN at the moment
 * the recv thread observes the shutdown (the point where the old code had
 * already close()d it), and closed only once disconnect() has joined every
 * thread that could touch it. */
class fd_probe_connection : public fss_connection {
public:
    static auto create(int t_fd) -> std::shared_ptr<fd_probe_connection>
    {
        auto conn = std::shared_ptr<fd_probe_connection>(new fd_probe_connection(t_fd));
        conn->startRecvThread(std::thread([conn]() -> void { conn->processMessages(); }));
        return conn;
    }
    ~fd_probe_connection() override = default;
    fd_probe_connection(const fd_probe_connection &) = delete;
    fd_probe_connection(fd_probe_connection &&) = delete;
    auto operator=(const fd_probe_connection &) -> fd_probe_connection & = delete;
    auto operator=(fd_probe_connection &&) -> fd_probe_connection & = delete;
    /* -1 until the recv thread observes teardown; then 1 if the descriptor was
     * still open at that moment (fcntl succeeded), 0 if it was already gone. */
    auto fdOpenAtShutdown() -> int { return this->fd_open_at_shutdown.load(); }
protected:
    explicit fd_probe_connection(int t_fd) : fss_connection(t_fd), raw_fd(t_fd) {}
    auto recvBytes(void *t_bytes, size_t t_max_bytes) -> ssize_t override
    {
        ssize_t got = fss_connection::recvBytes(t_bytes, t_max_bytes);
        if (got <= 0 && this->fd_open_at_shutdown.load() == -1)
        {
            this->fd_open_at_shutdown.store(::fcntl(this->raw_fd, F_GETFD) != -1 ? 1 : 0);
        }
        return got;
    }
private:
    int raw_fd;
    std::atomic<int> fd_open_at_shutdown{-1};
};

/* Wait until the connection's recv thread has delivered the closed message
 * (no handler is attached, so it lands in the poll queue). */
auto wait_for_closed(const std::shared_ptr<fss_connection> &conn) -> bool
{
    return fss_test::wait_for([&conn]() -> bool {
        auto msg = conn->getMsg();
        return msg != nullptr && msg->getType() == message_type_closed;
    });
}

} // namespace

TEST_CASE("transport: disconnect() closes the fd only after the recv thread is joined")
{
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    fss_test::scoped_fd peer(fds[0]);
    int raw_fd = fds[1];
    auto conn = fd_probe_connection::create(raw_fd);

    /* Let the recv thread reach its blocking recv() so the shutdown() wake-up
     * path is what gets exercised; the assertion holds either way (a not-yet-
     * blocked thread observes the retired fd and probes just the same). */
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    conn->disconnect();

    /* disconnect() joined the recv thread, so the probe it recorded is
     * visible: the descriptor had to still be open when the thread observed
     * the teardown — the old close-before-join ordering freed the fd number
     * for reuse at exactly that point. */
    REQUIRE(conn->fdOpenAtShutdown() == 1);
    /* ... and once disconnect() returns, the descriptor is really released. */
    errno = 0;
    REQUIRE(::fcntl(raw_fd, F_GETFD) == -1);
    REQUIRE(errno == EBADF);
}

TEST_CASE("transport: shutdownSocket() reserves the fd number until disconnect()")
{
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    fss_test::scoped_fd peer(fds[0]);
    int raw_fd = fds[1];
    auto conn = fd_probe_connection::create(raw_fd);

    conn->shutdownSocket();
    /* The recv thread unblocks and winds down, but the descriptor number must
     * stay reserved (shut down, not closed) until disconnect() has joined it. */
    REQUIRE(wait_for_closed(conn));
    REQUIRE(::fcntl(raw_fd, F_GETFD) != -1);

    conn->disconnect();
    errno = 0;
    REQUIRE(::fcntl(raw_fd, F_GETFD) == -1);
    REQUIRE(errno == EBADF);
}

TEST_CASE("transport: a recv-thread self-disconnect leaves the close to the owner")
{
    /* An oversized declared length makes the recv thread call disconnect() on
     * itself. It cannot join itself, so it must NOT close either: an owner-side
     * sender (the server's outbound worker) may still be mid-send. The close
     * belongs to the owner's later disconnect() (or the destructor). */
    fss_test::capture_cerr capture; /* swallow the expected oversized-frame ERROR */
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    fss_test::scoped_fd peer(fds[0]);
    int raw_fd = fds[1];
    auto conn = fd_probe_connection::create(raw_fd);

    uint16_t oversized = htons(FSS_MAX_MESSAGE_BYTES + 1);
    REQUIRE(::write(peer.get(), &oversized, sizeof(oversized)) == static_cast<ssize_t>(sizeof(oversized)));

    /* The closed message is queued after the self-disconnect completed, so the
     * fd state is settled once it arrives: shut down but still open. */
    REQUIRE(wait_for_closed(conn));
    REQUIRE(::fcntl(raw_fd, F_GETFD) != -1);

    conn->disconnect();
    errno = 0;
    REQUIRE(::fcntl(raw_fd, F_GETFD) == -1);
    REQUIRE(errno == EBADF);
}
