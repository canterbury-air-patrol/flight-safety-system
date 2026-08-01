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
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fss-client-ssl.hpp"
#include "fss-transport.hpp"
#include "test_helpers.hpp"

/* todo/66: the client used to fan a message out over its servers with a serial
 * loop of blocking sends on the caller's thread, so a server that completed
 * TLS and then stopped reading stalled telemetry to every healthy server
 * behind it for a whole TCP_USER_TIMEOUT (30 s by default). Each server now
 * owns an outbound worker; these cases pin that one wedged server can only
 * stall itself, that the per-connection id stamp (the todo/12 C8 invariant)
 * survives the fan-out becoming concurrent, and that a backlogged queue sheds
 * its oldest telemetry instead of growing. */

namespace {

namespace fss = flight_safety_system;

/* A connection whose send blocks until it is released, standing in for a peer
 * that completes TLS and then stops reading. shutdownSocket() is the release
 * hook — that is exactly what fss_server::disconnect() calls to unblock a
 * stalled worker before joining it, so overriding it here exercises the real
 * teardown path rather than a test-only escape. */
class BlackHoleConnection : public fss::transport::fss_connection {
    std::mutex block_lock{};
    std::condition_variable block_cv{};
    bool released{false};
public:
    BlackHoleConnection() = default;
    BlackHoleConnection(const BlackHoleConnection &) = delete;
    BlackHoleConnection(BlackHoleConnection &&) = delete;
    auto operator=(const BlackHoleConnection &) -> BlackHoleConnection & = delete;
    auto operator=(BlackHoleConnection &&) -> BlackHoleConnection & = delete;
    ~BlackHoleConnection() override { this->release(); }

    std::atomic<int> sends_entered{0};
    std::atomic<int> sends_completed{0};

    void release()
    {
        {
            std::scoped_lock guard(this->block_lock);
            this->released = true;
        }
        this->block_cv.notify_all();
    }
    void shutdownSocket() override { this->release(); }
protected:
    auto sendMsg(const std::shared_ptr<fss::transport::buf_len> &) -> bool override
    {
        this->sends_entered++;
        {
            std::unique_lock<std::mutex> guard(this->block_lock);
            this->block_cv.wait(guard, [this]() -> bool { return this->released; });
        }
        this->sends_completed++;
        return true;
    }
};

/* Records what the client actually put on the wire. The frames now arrive on
 * the server's outbound worker thread while the test reads them, so unlike the
 * single-threaded CapturingConnection in client.cpp this one publishes them
 * under a lock. */
class RecordingConnection : public fss::transport::fss_connection {
    mutable std::mutex sent_lock{};
    std::vector<std::shared_ptr<fss::transport::fss_message>> sent{};
public:
    RecordingConnection() = default;
    RecordingConnection(const RecordingConnection &) = delete;
    RecordingConnection(RecordingConnection &&) = delete;
    auto operator=(const RecordingConnection &) -> RecordingConnection & = delete;
    auto operator=(RecordingConnection &&) -> RecordingConnection & = delete;
    ~RecordingConnection() override = default;

    auto count() const -> size_t
    {
        std::scoped_lock guard(this->sent_lock);
        return this->sent.size();
    }
    auto at(size_t idx) const -> std::shared_ptr<fss::transport::fss_message>
    {
        std::scoped_lock guard(this->sent_lock);
        return idx < this->sent.size() ? this->sent[idx] : nullptr;
    }
protected:
    auto sendMsg(const std::shared_ptr<fss::transport::buf_len> &bl) -> bool override
    {
        if (auto decoded = fss::transport::fss_message::decode(bl))
        {
            std::scoped_lock guard(this->sent_lock);
            this->sent.push_back(decoded);
        }
        return true;
    }
};

/* fss_server with the protected setConnection exposed so a test can wire it to
 * one of the doubles above without a real socket. */
class InjectableServer : public fss::client_ssl::fss_server {
public:
    using fss_server::fss_server;
    using fss::transport::fss_message_cb::setConnection;
    InjectableServer(const InjectableServer &) = delete;
    InjectableServer(InjectableServer &&) = delete;
    auto operator=(const InjectableServer &) -> InjectableServer & = delete;
    auto operator=(InjectableServer &&) -> InjectableServer & = delete;
    ~InjectableServer() override = default;
};

/* fss_client with the protected addServer exposed. */
class FanoutClient : public fss::client_ssl::fss_client {
public:
    using fss_client::fss_client;
    FanoutClient(const FanoutClient &) = delete;
    FanoutClient(FanoutClient &&) = delete;
    auto operator=(const FanoutClient &) -> FanoutClient & = delete;
    auto operator=(FanoutClient &&) -> FanoutClient & = delete;
    ~FanoutClient() override = default;
    void add(const std::shared_ptr<fss::client_ssl::fss_server> &server) { this->addServer(server); }
    std::atomic<fss::client_ssl::connection_status> last_status{fss::client_ssl::CLIENT_CONNECTION_STATUS_UNKNOWN};
private:
    void connectionStatusChange(fss::client_ssl::connection_status status) override { this->last_status.store(status); }
};

/* A server whose dial blocks until released, standing in for a host that drops
 * SYNs or stalls the TLS handshake — the two cases that cost ~7 s and ~10 s
 * respectively on a real connect. */
class StallingDialServer : public fss::client_ssl::fss_server {
    std::mutex dial_lock{};
    std::condition_variable dial_cv{};
    bool released{false};
public:
    using fss_server::fss_server;
    StallingDialServer(const StallingDialServer &) = delete;
    StallingDialServer(StallingDialServer &&) = delete;
    auto operator=(const StallingDialServer &) -> StallingDialServer & = delete;
    auto operator=(StallingDialServer &&) -> StallingDialServer & = delete;
    ~StallingDialServer() override { this->release(); }

    std::atomic<int> dials_entered{0};

    void release()
    {
        {
            std::scoped_lock guard(this->dial_lock);
            this->released = true;
        }
        this->dial_cv.notify_all();
    }
    /* A real dial is bounded by the connect/handshake timeout, so a disconnect
     * that lands mid-dial completes once that expires. Release here so this
     * double has the same property: without it, disconnect() would join a
     * worker that nothing can ever wake. */
    void disconnect() override
    {
        this->release();
        fss_server::disconnect();
    }
protected:
    auto reconnect_to() -> bool override
    {
        this->dials_entered++;
        std::unique_lock<std::mutex> guard(this->dial_lock);
        this->dial_cv.wait(guard, [this]() -> bool { return this->released; });
        return false;
    }
};

/* A server whose dial succeeds immediately, installing a connection that
 * accepts whatever is written to it. */
class InstantDialServer : public fss::client_ssl::fss_server {
public:
    using fss_server::fss_server;
    InstantDialServer(const InstantDialServer &) = delete;
    InstantDialServer(InstantDialServer &&) = delete;
    auto operator=(const InstantDialServer &) -> InstantDialServer & = delete;
    auto operator=(InstantDialServer &&) -> InstantDialServer & = delete;
    ~InstantDialServer() override = default;

    std::atomic<int> dials_entered{0};
protected:
    auto reconnect_to() -> bool override
    {
        this->dials_entered++;
        this->setConnection(std::make_shared<RecordingConnection>());
        return true;
    }
};

auto make_position() -> std::shared_ptr<fss::transport::fss_message_position_report>
{
    return std::make_shared<fss::transport::fss_message_position_report>(-43.5, 172.5, 300U, 0U, 0U, int16_t{0}, 0U,
                                                                         std::string{"T"}, 0U, uint8_t{0}, 0U,
                                                                         uint8_t{0}, uint8_t{0}, uint64_t{0});
}

} // namespace

TEST_CASE("client: sendMsgAll does not stall healthy servers behind a black-holed one (todo/66)")
{
    auto client = std::make_shared<FanoutClient>();

    auto wedged_conn = std::make_shared<BlackHoleConnection>();
    auto wedged = std::make_shared<InjectableServer>(client.get(), "wedged.example", uint16_t{20200}, "", "", "");
    wedged->setConnection(wedged_conn);

    auto healthy_conn = std::make_shared<RecordingConnection>();
    auto healthy = std::make_shared<InjectableServer>(client.get(), "healthy.example", uint16_t{20201}, "", "", "");
    healthy->setConnection(healthy_conn);

    /* Wedged first, so it is ahead of the healthy server in the fan-out order:
     * pre-fix this is precisely the ordering that starved the healthy one. */
    client->add(wedged);
    client->add(healthy);

    auto start = std::chrono::steady_clock::now();
    client->sendMsgAll(make_position());
    auto elapsed = std::chrono::steady_clock::now() - start;

    /* The caller's thread must not have waited on the wedged server's send. */
    REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 500);

    /* The healthy server gets its telemetry while the other is still stuck. */
    REQUIRE(fss_test::wait_for([&]() -> bool { return healthy_conn->count() == 1; }));
    REQUIRE(healthy_conn->at(0)->getType() == fss::transport::message_type_position_report);
    REQUIRE(fss_test::wait_for([&]() -> bool { return wedged_conn->sends_entered.load() == 1; }));
    REQUIRE(wedged_conn->sends_completed.load() == 0);

    /* disconnect() shuts the socket down before joining, which is what releases
     * the stalled worker; if that order were wrong this would hang. */
    client->disconnect();
}

TEST_CASE("client: a concurrent fan-out still stamps each connection's own id (todo/12 C8)")
{
    /* sendMsgAll packs once and shares the frame read-only, so no fss_message
     * instance is sent on two connections. Each connection must still stamp its
     * own sequence id into its own copy. Give the two connections different
     * starting counters so a shared id would be unmistakable. */
    auto client = std::make_shared<FanoutClient>();

    auto conn_a = std::make_shared<RecordingConnection>();
    auto server_a = std::make_shared<InjectableServer>(client.get(), "a.example", uint16_t{20202}, "", "", "");
    server_a->setConnection(conn_a);

    auto conn_b = std::make_shared<RecordingConnection>();
    auto server_b = std::make_shared<InjectableServer>(client.get(), "b.example", uint16_t{20203}, "", "", "");
    server_b->setConnection(conn_b);

    /* Advance connection A's id counter to 3 before the fan-out. */
    for (int i = 0; i < 3; i++)
    {
        REQUIRE(conn_a->sendPacked(make_position()->getPacked()));
    }
    REQUIRE(conn_a->count() == 3);

    client->add(server_a);
    client->add(server_b);
    client->sendMsgAll(make_position());

    REQUIRE(fss_test::wait_for([&]() -> bool { return conn_a->count() == 4 && conn_b->count() == 1; }));
    REQUIRE(conn_a->at(3)->getId() == 4); // A's own fourth id
    REQUIRE(conn_b->at(0)->getId() == 1); // B's own first id

    client->disconnect();
}

TEST_CASE("client: a wedged server's outbound queue drops oldest and counts the loss (todo/66)")
{
    auto client = std::make_shared<FanoutClient>();

    auto wedged_conn = std::make_shared<BlackHoleConnection>();
    auto wedged = std::make_shared<InjectableServer>(client.get(), "wedged.example", uint16_t{20204}, "", "", "");
    wedged->setConnection(wedged_conn);
    client->add(wedged);

    /* Park the worker inside the first send, so everything queued after this
     * point stays in the bounded queue and the arithmetic below is exact. */
    client->sendMsgAll(make_position());
    REQUIRE(fss_test::wait_for([&]() -> bool { return wedged_conn->sends_entered.load() == 1; }));
    REQUIRE(wedged->getDroppedSends() == 0);

    constexpr int backlog = 20;
    constexpr uint64_t queue_cap = 8; // fss_server::max_pending_sends
    for (int i = 0; i < backlog; i++)
    {
        client->sendMsgAll(make_position());
    }
    REQUIRE(wedged->getDroppedSends() == backlog - queue_cap);

    client->disconnect();
}

TEST_CASE("client: an unreachable server does not delay reconnection to a healthy one (todo/66)")
{
    /* attemptReconnect() used to dial each entry in reconnect_servers in turn,
     * on the caller's thread, so a pass cost the SUM of every unreachable
     * server's connect/handshake timeout and a healthy server that dropped in
     * the same window waited behind all of them. */
    auto client = std::make_shared<FanoutClient>();

    auto stalling = std::make_shared<StallingDialServer>(client.get(), "black.example", uint16_t{20205}, "", "", "");
    auto healthy = std::make_shared<InstantDialServer>(client.get(), "good.example", uint16_t{20206}, "", "", "");
    /* Neither is connected, so both land in reconnect_servers — the stalling
     * one first, which is the ordering that starved the healthy one pre-fix. */
    client->add(stalling);
    client->add(healthy);

    auto start = std::chrono::steady_clock::now();
    client->attemptReconnect();
    auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 500);

    /* Both dials are in flight at once: the healthy server connects while the
     * other is still stuck in its connect(). */
    REQUIRE(fss_test::wait_for([&]() -> bool { return healthy->dials_entered.load() == 1; }));
    REQUIRE(fss_test::wait_for([&]() -> bool { return stalling->dials_entered.load() == 1; }));
    REQUIRE(healthy->connected());

    /* The pass after the dial lands harvests it into the live server list. The
     * server list stands in for the admitting server: since todo/79 a
     * connected server is reported as service only once it has admitted the
     * client, so the harvest alone no longer moves the status. Feeding it each
     * pass is harmless — admission is idempotent — and lets the status stay the
     * observable for "harvested AND admitted". */
    REQUIRE(fss_test::wait_for([&]() -> bool {
        client->attemptReconnect();
        healthy->processMessage(std::make_shared<fss::transport::fss_message_server_list>());
        return client->last_status.load() == fss::client_ssl::CLIENT_CONNECTION_STATUS_CONNECTED_1_SERVER;
    }));

    /* Being in the live list is what matters: telemetry now reaches it. The
     * connection already carries the version + identity handshake reconnect()
     * sent, so count from there rather than from zero. */
    auto healthy_conn = std::dynamic_pointer_cast<RecordingConnection>(healthy->getConnection());
    REQUIRE(healthy_conn != nullptr);
    const size_t handshake_frames = healthy_conn->count();
    REQUIRE(handshake_frames == 2);
    REQUIRE(healthy_conn->at(0)->getType() == fss::transport::message_type_version);

    client->sendMsgAll(make_position());
    REQUIRE(fss_test::wait_for([&]() -> bool { return healthy_conn->count() == handshake_frames + 1; }));
    REQUIRE(healthy_conn->at(handshake_frames)->getType() == fss::transport::message_type_position_report);

    client->disconnect();
}

TEST_CASE("client: repeated attemptReconnect does not pile up dials on a stalled server (todo/66)")
{
    auto client = std::make_shared<FanoutClient>();
    auto stalling = std::make_shared<StallingDialServer>(client.get(), "black.example", uint16_t{20207}, "", "", "");
    client->add(stalling);

    client->attemptReconnect();
    REQUIRE(fss_test::wait_for([&]() -> bool { return stalling->dials_entered.load() == 1; }));

    /* An application driving reconnection at 1 Hz keeps calling while the dial
     * is stuck. Each call must be a no-op, not another queued attempt. */
    for (int i = 0; i < 20; i++)
    {
        client->attemptReconnect();
    }
    REQUIRE(stalling->dials_entered.load() == 1);

    client->disconnect();
}
