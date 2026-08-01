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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "fss-client-ssl.hpp"
#include "fss-transport.hpp"
#include "test_helpers.hpp"

/* todo/79: the client derived its connection status from the size of the live
 * server list, and a server joined that list the moment its TCP/TLS connect
 * succeeded. Admission is a later, server-side decision — the server refuses a
 * client while its DB fail-safe is degraded, and refuses a duplicate identity,
 * both after the handshake the client already counted as success. So a
 * connection the server severed milliseconds later was reported as
 * CONNECTED_1_SERVER and then immediately as DISCONNECTED, which cap-fmu reads
 * as comms-restored: it left its comms-loss failsafe, resumed the previous
 * command, and re-entered the failsafe when the drop landed (CAP/test-plan
 * Path M m05 — the aircraft left its safety state twice in seventeen seconds).
 *
 * These cases pin that a server counts as service only once it has admitted
 * the client, that each of the three admission signals works on its own, that
 * RTT is deliberately not one of them, and that admission does not survive the
 * connection that earned it. */

namespace {

namespace fss = flight_safety_system;

/* A connection that swallows whatever is written to it. The tests here care
 * about what the client reports, not what it sends. */
class SilentConnection : public fss::transport::fss_connection {
public:
    SilentConnection() = default;
    SilentConnection(const SilentConnection &) = delete;
    SilentConnection(SilentConnection &&) = delete;
    auto operator=(const SilentConnection &) -> SilentConnection & = delete;
    auto operator=(SilentConnection &&) -> SilentConnection & = delete;
    ~SilentConnection() override = default;
protected:
    auto sendMsg(const std::shared_ptr<fss::transport::buf_len> &) -> bool override { return true; }
};

/* fss_server with setConnection exposed, so a test can present a server that
 * is connected without a real socket — which is exactly the state todo/79 is
 * about: connected, not yet admitted. */
class AdmissionServer : public fss::client_ssl::fss_server {
public:
    using fss_server::fss_server;
    using fss::transport::fss_message_cb::setConnection;
    AdmissionServer(const AdmissionServer &) = delete;
    AdmissionServer(AdmissionServer &&) = delete;
    auto operator=(const AdmissionServer &) -> AdmissionServer & = delete;
    auto operator=(AdmissionServer &&) -> AdmissionServer & = delete;
    ~AdmissionServer() override = default;
};

/* Records every status the client reports, in order, so a case can assert the
 * *sequence* of transitions rather than just the final state. The flap this
 * fixes is a sequence: CONNECTED then DISCONNECTED 1 ms apart. */
class AdmissionClient : public fss::client_ssl::fss_client {
    mutable std::mutex status_lock{};
    std::vector<fss::client_ssl::connection_status> statuses{};
public:
    using fss_client::fss_client;
    AdmissionClient(const AdmissionClient &) = delete;
    AdmissionClient(AdmissionClient &&) = delete;
    auto operator=(const AdmissionClient &) -> AdmissionClient & = delete;
    auto operator=(AdmissionClient &&) -> AdmissionClient & = delete;
    ~AdmissionClient() override = default;

    void add(const std::shared_ptr<fss::client_ssl::fss_server> &server) { this->addServer(server); }
    auto count() const -> size_t
    {
        std::scoped_lock guard(this->status_lock);
        return this->statuses.size();
    }
    auto at(size_t idx) const -> fss::client_ssl::connection_status
    {
        std::scoped_lock guard(this->status_lock);
        return idx < this->statuses.size() ? this->statuses[idx] : fss::client_ssl::CLIENT_CONNECTION_STATUS_UNKNOWN;
    }
private:
    void connectionStatusChange(fss::client_ssl::connection_status status) override
    {
        std::scoped_lock guard(this->status_lock);
        this->statuses.push_back(status);
    }
};

/* A server that is connected — the state a successful dial leaves it in. */
auto make_connected_server(AdmissionClient *client, uint16_t port) -> std::shared_ptr<AdmissionServer>
{
    auto server = std::make_shared<AdmissionServer>(client, "server.example", port, "", "", "");
    server->setConnection(std::make_shared<SilentConnection>());
    return server;
}

/* A server whose dial succeeds immediately. Reaching the live list by the
 * reconnect path rather than by addServer() matters for the m05 case below:
 * attemptReconnect()'s final phase is what reports the status after a
 * successful dial, and that report — made on the strength of the dial alone —
 * is the spurious comms-restored edge todo/79 removes. */
class DiallingServer : public fss::client_ssl::fss_server {
public:
    using fss_server::fss_server;
    DiallingServer(const DiallingServer &) = delete;
    DiallingServer(DiallingServer &&) = delete;
    auto operator=(const DiallingServer &) -> DiallingServer & = delete;
    auto operator=(DiallingServer &&) -> DiallingServer & = delete;
    ~DiallingServer() override = default;

    std::atomic<int> dials{0};
protected:
    auto reconnect_to() -> bool override
    {
        this->dials++;
        this->setConnection(std::make_shared<SilentConnection>());
        return true;
    }
};

auto make_server_list() -> std::shared_ptr<fss::transport::fss_message>
{
    return std::make_shared<fss::transport::fss_message_server_list>();
}

auto make_command() -> std::shared_ptr<fss::transport::fss_message>
{
    return std::make_shared<fss::transport::fss_message_asset_command>(fss::transport::asset_command_rtl, uint64_t{0});
}

auto make_smm_settings() -> std::shared_ptr<fss::transport::fss_message>
{
    return std::make_shared<fss::transport::fss_message_smm_settings>("smm.example", fss::secure_string{"user"},
                                                                      fss::secure_string{"pass"});
}

} // namespace

TEST_CASE("client: a connection the server never admits is never reported as service (todo/79)")
{
    /* The m05 regression, by the path that actually produced it. The dial
     * succeeds, attemptReconnect() harvests it into the live list and reports
     * the resulting status; the server then refuses the client at admission
     * (DB fail-safe degraded, or duplicate identity) and severs, having sent
     * nothing at all in between — the fail-safe refusal returns before the
     * client is ever activated, so it receives no message.
     *
     * Pre-fix the harvest reported CONNECTED_1_SERVER on the strength of the
     * dial, and the sever reported DISCONNECTED milliseconds later. cap-fmu
     * reads that pair as comms-restored: it left its comms-loss failsafe,
     * resumed the previous command, and re-entered the failsafe when the drop
     * landed. */
    auto client = std::make_shared<AdmissionClient>();
    auto server = std::make_shared<DiallingServer>(client.get(), "server.example", uint16_t{20301}, "", "", "");
    client->add(server);

    client->attemptReconnect();
    REQUIRE(fss_test::wait_for([&]() -> bool { return server->dials.load() == 1; }));
    client->attemptReconnect();
    REQUIRE(server->connected());

    /* Connected and harvested, but the server has said nothing. Whatever was
     * reported, none of it may be service. */
    for (size_t i = 0; i < client->count(); i++)
    {
        REQUIRE(client->at(i) == fss::client_ssl::CLIENT_CONNECTION_STATUS_DISCONNECTED);
    }

    /* The refusal lands. */
    server->processMessage(std::make_shared<fss::transport::fss_message_closed>());

    /* Still nothing but DISCONNECTED: no spurious comms-restored edge for the
     * aircraft to act on (CAP/test-plan TC-FS-003). */
    REQUIRE(client->count() > 0);
    for (size_t i = 0; i < client->count(); i++)
    {
        REQUIRE(client->at(i) == fss::client_ssl::CLIENT_CONNECTION_STATUS_DISCONNECTED);
    }
}

TEST_CASE("client: a server list admits the connection (todo/79)")
{
    auto client = std::make_shared<AdmissionClient>();
    auto server = make_connected_server(client.get(), uint16_t{20302});
    client->add(server);

    server->processMessage(make_server_list());
    REQUIRE(client->count() == 1);
    REQUIRE(client->at(0) == fss::client_ssl::CLIENT_CONNECTION_STATUS_CONNECTED_1_SERVER);

    server->processMessage(std::make_shared<fss::transport::fss_message_closed>());
    REQUIRE(client->count() == 2);
    REQUIRE(client->at(1) == fss::client_ssl::CLIENT_CONNECTION_STATUS_DISCONNECTED);
}

TEST_CASE("client: a command admits the connection (todo/79)")
{
    /* Independently sufficient: the identify path skips the server-list send
     * when the active-server read fails, so the command an aircraft is
     * carrying can be the first thing that proves admission. */
    auto client = std::make_shared<AdmissionClient>();
    auto server = make_connected_server(client.get(), uint16_t{20303});
    client->add(server);

    server->processMessage(make_command());
    REQUIRE(client->count() == 1);
    REQUIRE(client->at(0) == fss::client_ssl::CLIENT_CONNECTION_STATUS_CONNECTED_1_SERVER);
}

TEST_CASE("client: SMM settings admit the connection (todo/79)")
{
    auto client = std::make_shared<AdmissionClient>();
    auto server = make_connected_server(client.get(), uint16_t{20304});
    client->add(server);

    server->processMessage(make_smm_settings());
    REQUIRE(client->count() == 1);
    REQUIRE(client->at(0) == fss::client_ssl::CLIENT_CONNECTION_STATUS_CONNECTED_1_SERVER);
}

TEST_CASE("client: an RTT request alone does not admit the connection (todo/79)")
{
    /* Deliberate, and the reason the signal is not simply "any message". The
     * server sends RTT requests to every client in its list, not to identified
     * ones only, so a client whose identify is about to be refused as a
     * duplicate (todo/31) can receive one first. Counting that as service would
     * close the flap for the fail-safe case and leave it open for the duplicate
     * case. */
    auto client = std::make_shared<AdmissionClient>();
    auto server = make_connected_server(client.get(), uint16_t{20305});
    client->add(server);

    server->processMessage(std::make_shared<fss::transport::fss_message_rtt_request>());
    REQUIRE(client->count() == 0);

    server->processMessage(std::make_shared<fss::transport::fss_message_closed>());
    REQUIRE(client->count() == 1);
    REQUIRE(client->at(0) == fss::client_ssl::CLIENT_CONNECTION_STATUS_DISCONNECTED);
}

TEST_CASE("client: repeated admission signals report the status once (todo/79)")
{
    /* The signals are ordinary traffic and keep arriving; only the false->true
     * edge is a status change. */
    auto client = std::make_shared<AdmissionClient>();
    auto server = make_connected_server(client.get(), uint16_t{20306});
    client->add(server);

    for (int i = 0; i < 5; i++)
    {
        server->processMessage(make_server_list());
        server->processMessage(make_command());
    }
    REQUIRE(client->count() == 1);
    REQUIRE(client->at(0) == fss::client_ssl::CLIENT_CONNECTION_STATUS_CONNECTED_1_SERVER);
}

TEST_CASE("client: only admitted servers count towards the status (todo/79)")
{
    /* Two servers connected, one admitted: one server's worth of service. */
    auto client = std::make_shared<AdmissionClient>();
    auto admitted = make_connected_server(client.get(), uint16_t{20307});
    auto silent = make_connected_server(client.get(), uint16_t{20308});
    client->add(admitted);
    client->add(silent);

    admitted->processMessage(make_server_list());
    REQUIRE(client->count() == 1);
    REQUIRE(client->at(0) == fss::client_ssl::CLIENT_CONNECTION_STATUS_CONNECTED_1_SERVER);

    silent->processMessage(make_server_list());
    REQUIRE(client->count() == 2);
    REQUIRE(client->at(1) == fss::client_ssl::CLIENT_CONNECTION_STATUS_CONNECTED_2_OR_MORE);
}

TEST_CASE("client: admission does not survive the connection that earned it (todo/79)")
{
    /* A reconnect into a server that now refuses the client must not inherit
     * the previous session's admission — that would restore exactly the
     * behaviour this removes. */
    auto client = std::make_shared<AdmissionClient>();
    auto server = make_connected_server(client.get(), uint16_t{20309});
    client->add(server);

    server->processMessage(make_server_list());
    REQUIRE(client->at(0) == fss::client_ssl::CLIENT_CONNECTION_STATUS_CONNECTED_1_SERVER);
    server->processMessage(std::make_shared<fss::transport::fss_message_closed>());
    REQUIRE(client->at(1) == fss::client_ssl::CLIENT_CONNECTION_STATUS_DISCONNECTED);
    const size_t after_drop = client->count();

    /* The server is back in reconnect_servers with a live connection again,
     * and this time it says nothing before severing. */
    server->setConnection(std::make_shared<SilentConnection>());
    server->processMessage(std::make_shared<fss::transport::fss_message_closed>());
    for (size_t i = after_drop; i < client->count(); i++)
    {
        REQUIRE(client->at(i) == fss::client_ssl::CLIENT_CONNECTION_STATUS_DISCONNECTED);
    }
}
