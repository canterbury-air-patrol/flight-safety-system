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
