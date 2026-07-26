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

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "fss-client-ssl.hpp"
#include "fss-transport.hpp"

/* todo/67: updateServers() only ever ADDED. There was no removal path anywhere
 * in client-ssl.cpp — entries migrated between `servers` and
 * `reconnect_servers` forever and never left either — so a server deactivated
 * in config_serverconfig dropped out of every broadcast list while every client
 * that had ever seen it kept it, and kept paying a blocking connect attempt for
 * it every backoff interval. Silently, too: nothing logged that the client was
 * carrying dead servers. These cases pin the maintenance the list now gets. */

namespace {

namespace fss = flight_safety_system;

struct FakeClock : public fss::IClock {
    uint64_t t{0};
    auto now_ms() const -> uint64_t override { return t; }
    void advance(uint64_t ms) { t += ms; }
};

/* Exposes the configuration setters, which are protected because they are meant
 * to be driven from a config file. Nothing here ever dials, so every server
 * stays in the pending-reconnect list — which is where the expiry work is
 * visible without needing a socket. */
class ListClient : public fss::client_ssl::fss_client {
public:
    using fss_client::fss_client;
    ListClient(const ListClient &) = delete;
    ListClient(ListClient &&) = delete;
    auto operator=(const ListClient &) -> ListClient & = delete;
    auto operator=(ListClient &&) -> ListClient & = delete;
    ~ListClient() override = default;
    void setExpiry(uint64_t ms) { this->setLearnedServerExpiryMs(ms); }
    void setCap(size_t n) { this->setMaxLearnedServers(n); }
};

auto make_list(const std::vector<std::pair<std::string, uint16_t>> &entries)
    -> std::shared_ptr<fss::transport::fss_message_server_list>
{
    auto msg = std::make_shared<fss::transport::fss_message_server_list>();
    for (const auto &entry : entries)
    {
        msg->addServer(entry.first, entry.second);
    }
    return msg;
}

constexpr uint64_t expiry_window_ms = 60000;
constexpr uint64_t broadcast_interval_ms = 15000;

} // namespace

TEST_CASE("client: a learned server absent from later lists is expired (todo/67)")
{
    auto client = std::make_shared<ListClient>();
    auto clock = std::make_shared<FakeClock>();
    client->setClock(clock);
    client->setExpiry(expiry_window_ms);

    client->updateServers(make_list({{"learned.example", 20200}, {"other.example", 20201}}));
    REQUIRE(client->getServerCount() == 2);
    REQUIRE(client->getLearnedServerCount() == 2);

    /* While it keeps being advertised the window never starts, however long the
     * client runs. */
    for (int round = 0; round < 10; round++)
    {
        clock->advance(broadcast_interval_ms);
        client->updateServers(make_list({{"learned.example", 20200}, {"other.example", 20201}}));
    }
    REQUIRE(client->getServerCount() == 2);

    /* It is decommissioned: it drops out of the broadcast. Nothing goes until
     * the whole window has passed without a sighting. */
    clock->advance(expiry_window_ms - 1000);
    client->updateServers(make_list({{"other.example", 20201}}));
    REQUIRE(client->getServerCount() == 2);

    clock->advance(2000); // now past the window since it was last seen
    client->updateServers(make_list({{"other.example", 20201}}));
    REQUIRE(client->getServerCount() == 1);
    REQUIRE(client->getLearnedServerCount() == 1);

    /* attemptReconnect() is what tears the expired entry down; it must not
     * resurrect it or disturb the survivor. */
    client->attemptReconnect();
    REQUIRE(client->getServerCount() == 1);
}

TEST_CASE("client: a config-file server is never expired (todo/67)")
{
    /* The operator's declared intent must survive an outage that empties every
     * broadcast list — otherwise a client that lost contact during a server
     * restart would prune away the only address it could come back on. */
    auto client = std::make_shared<ListClient>();
    auto clock = std::make_shared<FakeClock>();
    client->setClock(clock);
    client->setExpiry(expiry_window_ms);

    client->connectTo("configured.example", uint16_t{20202}, /*connect*/ false);
    client->updateServers(make_list({{"learned.example", 20203}}));
    REQUIRE(client->getServerCount() == 2);
    REQUIRE(client->getLearnedServerCount() == 1);

    for (int round = 0; round < 20; round++)
    {
        clock->advance(broadcast_interval_ms);
        client->updateServers(make_list({}));
        client->attemptReconnect();
    }

    /* The learned one is gone; the configured one is untouched. */
    REQUIRE(client->getServerCount() == 1);
    REQUIRE(client->getLearnedServerCount() == 0);
}

TEST_CASE("client: a learned server seen again inside the window is not expired (todo/67)")
{
    auto client = std::make_shared<ListClient>();
    auto clock = std::make_shared<FakeClock>();
    client->setClock(clock);
    client->setExpiry(expiry_window_ms);

    client->updateServers(make_list({{"learned.example", 20204}}));
    REQUIRE(client->getServerCount() == 1);

    /* Absent from one broadcast, present in the next: the sighting resets the
     * window, so twice the window's worth of elapsed time expires nothing. */
    clock->advance(expiry_window_ms - 10000);
    client->updateServers(make_list({}));
    clock->advance(expiry_window_ms - 10000);
    client->updateServers(make_list({{"learned.example", 20204}}));
    clock->advance(expiry_window_ms - 10000);
    client->updateServers(make_list({{"learned.example", 20204}}));
    REQUIRE(client->getServerCount() == 1);
}

TEST_CASE("client: an empty list from one broadcast round expires nothing early (todo/67)")
{
    /* Server-side, a failed read of config_serverconfig returns nullopt and the
     * poller keeps its previous cache, so a read outage cannot broadcast an
     * empty list. This pins the client side of that contract anyway: even if an
     * empty list did arrive, one round is not enough to drop anything. */
    auto client = std::make_shared<ListClient>();
    auto clock = std::make_shared<FakeClock>();
    client->setClock(clock);
    client->setExpiry(expiry_window_ms);

    client->updateServers(make_list({{"learned.example", 20205}}));
    clock->advance(broadcast_interval_ms);
    client->updateServers(make_list({}));
    clock->advance(broadcast_interval_ms);
    client->updateServers(make_list({}));
    REQUIRE(client->getServerCount() == 1);
}

TEST_CASE("client: expiry can be disabled (todo/67)")
{
    /* 0 keeps the pre-todo/67 behaviour for a deployment that wants it. */
    auto client = std::make_shared<ListClient>();
    auto clock = std::make_shared<FakeClock>();
    client->setClock(clock);
    client->setExpiry(0);

    client->updateServers(make_list({{"learned.example", 20206}}));
    clock->advance(100000000);
    client->updateServers(make_list({}));
    client->attemptReconnect();
    REQUIRE(client->getServerCount() == 1);
}

TEST_CASE("client: the learned-server cap refuses further entries (todo/67)")
{
    auto client = std::make_shared<ListClient>();
    auto clock = std::make_shared<FakeClock>();
    client->setClock(clock);
    client->setCap(4);

    std::vector<std::pair<std::string, uint16_t>> entries;
    for (uint16_t i = 0; i < 10; i++)
    {
        entries.emplace_back("learned" + std::to_string(i) + ".example", static_cast<uint16_t>(20300 + i));
    }
    client->updateServers(make_list(entries));
    REQUIRE(client->getLearnedServerCount() == 4);

    /* Re-advertising the same list must not grow it past the cap either. */
    client->updateServers(make_list(entries));
    REQUIRE(client->getLearnedServerCount() == 4);
}

TEST_CASE("client: config-file servers do not count against the learned cap (todo/67)")
{
    auto client = std::make_shared<ListClient>();
    client->setCap(2);
    client->connectTo("configured1.example", uint16_t{20400}, false);
    client->connectTo("configured2.example", uint16_t{20401}, false);
    client->connectTo("configured3.example", uint16_t{20402}, false);

    client->updateServers(make_list({{"learned1.example", 20403}, {"learned2.example", 20404}}));
    REQUIRE(client->getLearnedServerCount() == 2);
    REQUIRE(client->getServerCount() == 5);
}

TEST_CASE("client: config keys set the expiry window and the cap (todo/67)")
{
    const char *tmppath = "client-server-list-test.json";
    {
        std::ofstream cfg(tmppath);
        cfg << R"({"name":"test-asset","learned_server_expiry_ms":1234,"max_learned_servers":3,)"
            << R"("ssl":{"ca_public_key":"","client_private_key":"","client_public_key":""},"servers":[]})";
    }
    fss::client_ssl::fss_client client(tmppath);
    REQUIRE(client.getLearnedServerExpiryMs() == 1234);
    REQUIRE(client.getMaxLearnedServers() == 3);
    std::remove(tmppath);
}

TEST_CASE("client: expiry and cap defaults are the documented ones (todo/67)")
{
    fss::client_ssl::fss_client client;
    REQUIRE(client.getLearnedServerExpiryMs() == fss::client_ssl::default_learned_server_expiry_ms);
    REQUIRE(client.getMaxLearnedServers() == fss::client_ssl::default_max_learned_servers);
}
