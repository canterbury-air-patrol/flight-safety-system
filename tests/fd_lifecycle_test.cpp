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
    /* Simulates the object-reuse reconnect pattern fss_connection::connectTo()
     * itself uses on this base class (fd == -1 check, then a fresh socket on
     * the SAME instance) — exposes the protected setFd() so a test can set up
     * the reuse-with-a-stale-pending-close scenario. */
    void reuseFd(int new_fd) { this->setFd(new_fd); }
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

TEST_CASE("transport: shutdownSocket() closes a stale pending fd when the object is reused")
{
    /* fss_connection::connectTo() reuses the same object for a reconnect (an
     * fd == -1 check, then a fresh socket) rather than allocating a new
     * fss_connection. If an earlier session's disconnect() left its close
     * pending (self-disconnect or a failed join — see the previous test), the
     * next shutdownSocket() on the reused object must roll that stale
     * descriptor over into a real close rather than leaking it. */
    int fds_a[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds_a) == 0);
    fss_test::scoped_fd peer_a(fds_a[0]);
    int raw_fd_a = fds_a[1];
    auto conn = fd_probe_connection::create(raw_fd_a);

    conn->shutdownSocket();
    REQUIRE(wait_for_closed(conn));
    /* Session A's fd is shut down but deliberately left open — the state a
     * self-disconnect or failed join leaves behind, with nobody left to call
     * disconnect() for that session again. */
    REQUIRE(::fcntl(raw_fd_a, F_GETFD) != -1);

    int fds_b[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds_b) == 0);
    fss_test::scoped_fd peer_b(fds_b[0]);
    int raw_fd_b = fds_b[1];
    conn->reuseFd(raw_fd_b);

    conn->shutdownSocket();
    errno = 0;
    REQUIRE(::fcntl(raw_fd_a, F_GETFD) == -1);
    REQUIRE(errno == EBADF);
    /* B's own fd is shut down but not yet closed — same deferred-close
     * contract as every other session. */
    REQUIRE(::fcntl(raw_fd_b, F_GETFD) != -1);

    conn->disconnect();
    errno = 0;
    REQUIRE(::fcntl(raw_fd_b, F_GETFD) == -1);
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
