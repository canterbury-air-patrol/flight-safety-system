#include "fss-transport.hpp"
#include "fss-transport-ssl.hpp"
#include "fss-log.hpp"
#include "fss.hpp"
#include "fss-server.hpp"

#include <algorithm>
#include <iostream>
#include <fstream>
#include <csignal>
#include <list>
#include <memory>
#include <mutex>
#include <atomic>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
#include <json/json.h>
#pragma GCC diagnostic pop

#include <unistd.h>

namespace flight_safety_system {
namespace server {
/* Defined in client_session.cpp. Exposed here so the periodic broadcast
 * of the active server list can reuse the helper. */
auto build_server_list_msg(IDatabase *dbc) -> std::shared_ptr<transport::fss_message_server_list>;
} // namespace server
} // namespace flight_safety_system

class server_clients : public flight_safety_system::server::fss_client_handler {
private:
    std::mutex lock{};
    std::list<std::shared_ptr<flight_safety_system::server::fss_client>> clients{};
    std::queue<std::shared_ptr<flight_safety_system::server::fss_client>> disconnected{};
    uint32_t total_clients{0};
    std::atomic<bool> shutting_down{false};
    uint64_t client_timeout_ms{30000};
public:
    server_clients() = default;
    ~server_clients() override {
        this->shutting_down = true;
        std::lock_guard<std::mutex> guard(this->lock);
        for (const auto &c: this->clients)
        {
            c->disconnect();
        }
    }
    server_clients(server_clients&) = delete;
    server_clients(server_clients&&) = delete;
    auto operator=(server_clients&) -> server_clients& = delete;
    auto operator=(server_clients&&) -> server_clients& = delete;
    void cleanupRemovableClients()
    {
        std::lock_guard<std::mutex> guard(this->lock);
        while(!this->disconnected.empty())
        {
            auto client = this->disconnected.front();
            this->disconnected.pop();
            total_clients--;
        }
    };
    void setClientTimeoutMs(uint64_t ms) { this->client_timeout_ms = ms; }
    void clientConnected(std::shared_ptr<flight_safety_system::server::fss_client> client)
    {
        client->setTimeoutMs(this->client_timeout_ms);
        std::lock_guard<std::mutex> guard(this->lock);
        this->total_clients++;
        this->clients.push_back(std::move(client));
    };
    void clientDisconnected(flight_safety_system::server::fss_client *client) override
    {
        std::lock_guard<std::mutex> guard(this->lock);
        if (this->shutting_down) return;
        auto it = std::find_if(this->clients.begin(), this->clients.end(), [client](const auto &c) -> auto {
            return c.get() == client;
        });
        if (it != this->clients.end())
        {
            this->disconnected.push(*it);
            this->clients.erase(it);
        }
    };
    void broadcastMsg(const std::shared_ptr<flight_safety_system::transport::fss_message> &msg, flight_safety_system::server::fss_client *except = nullptr) override
    {
        std::lock_guard<std::mutex> guard(this->lock);
        for (const auto &client : this->clients)
        {
            if (client->isAircraft() && client.get() != except)
            {
                client->sendMsg(msg);
            }
        }
    }
    void checkTimeouts()
    {
        std::vector<std::shared_ptr<flight_safety_system::server::fss_client>> snapshot;
        {
            std::lock_guard<std::mutex> guard(this->lock);
            std::copy(this->clients.begin(), this->clients.end(), std::back_inserter(snapshot));
        }
        for (const auto &client : snapshot)
        {
            if (client->isTimedOut())
            {
                FSS_LOG_WARN("server", "Client timed out, disconnecting");
                this->clientDisconnected(client.get());
            }
        }
    };
    void sendSMMSettings()
    {
        std::lock_guard<std::mutex> guard(this->lock);
        for(const auto &client: this->clients)
        {
            client->sendSMMSettings();
        }
    };
    void sendRTTRequest(const std::shared_ptr<flight_safety_system::transport::fss_message_rtt_request> &rtt_req)
    {
        std::lock_guard<std::mutex> guard(this->lock);
        for(const auto &client: this->clients)
        {
            client->sendRTTRequest(rtt_req);
        }
    };
    void sendCommand()
    {
        std::lock_guard<std::mutex> guard(this->lock);
        for(const auto &client: this->clients)
        {
            client->sendCommand();
        }
    };
};

volatile sig_atomic_t running = 1;

void sigIntHandler(int signum __attribute__((unused)))
{
    running = 0;
}

auto
main(int argc, char *argv[]) -> int
{
    struct sigaction sa_int = {};
    sa_int.sa_handler = sigIntHandler;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = 0;
    sigaction(SIGINT, &sa_int, nullptr);
    struct sigaction sa_pipe = {};
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sa_pipe.sa_flags = 0;
    sigaction(SIGPIPE, &sa_pipe, nullptr);
    std::string conf_file = (argc > 1 ? std::string(argv[1]) : "/etc/fss/server.json");
    std::ifstream configfile(conf_file);
    if (!configfile.is_open())
    {
        FSS_LOG_ERROR("server", "Failed to load configuration: " << conf_file);
        exit(-1);
    }
    Json::Value config;
    configfile >> config;

    if (config.isMember("log_level"))
    {
        flight_safety_system::log::set_level(config["log_level"].asString());
    }

    auto dbc = std::make_shared<flight_safety_system::server::db_connection>(config["postgres"]["host"].asString(), config["postgres"]["user"].asString(), config["postgres"]["pass"].asString(), config["postgres"]["db"].asString());

    constexpr std::size_t default_db_queue_depth = 10000;
    std::size_t db_queue_depth = config.isMember("db_queue_depth") ? config["db_queue_depth"].asUInt() : default_db_queue_depth;
    flight_safety_system::server::db_write_sink sink =
        [dbc](const flight_safety_system::server::db_write_task &task) -> void {
            std::visit(flight_safety_system::server::overloaded{
                [&](const flight_safety_system::server::rtt_write &w) -> void {
                    dbc->recordRtt(w.asset_id, w.rtt_ms);
                },
                [&](const flight_safety_system::server::position_write &w) -> void {
                    dbc->recordPosition(w.asset_id, w.latitude, w.longitude, w.altitude);
                },
                [&](const flight_safety_system::server::status_write &w) -> void {
                    dbc->recordStatus(w.asset_id, w.bat_percent, w.bat_mah_used, w.bat_voltage);
                },
                [&](const flight_safety_system::server::search_status_write &w) -> void {
                    dbc->recordSearchStatus(w.asset_id, w.search_id, w.completed, w.total);
                },
            }, task);
        };
    auto writer = std::make_shared<flight_safety_system::server::db_write_queue>(db_queue_depth, sink);

    auto clients = std::make_shared<server_clients>();
    constexpr int default_client_timeout_sec = 30;
    constexpr int msec_per_sec = 1000;
    uint64_t client_timeout_sec = config.isMember("client_timeout") ? config["client_timeout"].asUInt64() : default_client_timeout_sec;
    clients->setClientTimeoutMs(client_timeout_sec * msec_per_sec);

    std::shared_ptr<flight_safety_system::transport::fss_listen> listen;
    FSS_LOG_INFO("server", "Starting fss server in TLS mode");
    std::string ca_public_key = config["ssl"]["ca_public_key"].asString();
    std::string server_private_key = config["ssl"]["server_private_key"].asString();
    std::string server_public_key = config["ssl"]["server_public_key"].asString();
    if (ca_public_key == "" || server_private_key == "" || server_public_key == "")
    {
        FSS_LOG_ERROR("server", "Missing ssl parameter, all of these are required: 'ca_public_key', 'server_private_key', 'server_public_key'");
        exit(-1);
    }
    std::string crl_file = config["ssl"].isMember("crl_file") ? config["ssl"]["crl_file"].asString() : std::string{};
    if (!crl_file.empty())
    {
        FSS_LOG_INFO("server", "CRL file configured: " << crl_file);
    }
    listen = std::make_shared<flight_safety_system::transport_ssl::fss_listen>(config["port"].asInt(),
        [dbc, writer, &clients](std::shared_ptr<flight_safety_system::transport::fss_connection> conn) -> bool {
#ifdef DEBUG
            std::cout << "New client connected" << std::endl;
#endif
            clients->clientConnected(std::make_shared<flight_safety_system::server::fss_client>(std::move(conn), dbc.get(), writer, clients.get()));
            return true;
        },
        config["ssl"]["ca_public_key"].asString(), config["ssl"]["server_private_key"].asString(), config["ssl"]["server_public_key"].asString(), crl_file);

    /* Split tick: sendCommand runs every command_poll_ms so safety-critical
     * commands (TERM, DISARM) reach aircraft in <=100ms instead of <=1s.
     * Per-second tasks (RTT, timeout sweep) and per-15s tasks (server list,
     * SMM settings) retain their original cadence via the tick counter. */
    constexpr int command_poll_ms = flight_safety_system::server::command_poll_ms;
    static_assert(1000 % command_poll_ms == 0, "command_poll_ms must evenly divide 1000 to avoid tick skew");
    static_assert(1000 / command_poll_ms > 0, "command_poll_ms must be <= 1000ms");

    constexpr int usec_per_msec = 1000;
    constexpr int ticks_per_sec = 1000 / command_poll_ms;
    constexpr int send_config_period_ticks = 15 * ticks_per_sec;
    uint64_t tick_counter = 0;
    while (running == 1)
    {
        usleep(command_poll_ms * usec_per_msec);
        clients->sendCommand();
        if ((tick_counter % ticks_per_sec) == 0)
        {
            clients->cleanupRemovableClients();
            clients->checkTimeouts();
            auto rtt_req = std::make_shared<flight_safety_system::transport::fss_message_rtt_request>();
            clients->sendRTTRequest(rtt_req);
        }
        if ((tick_counter % send_config_period_ticks) == 0)
        {
            clients->broadcastMsg(flight_safety_system::server::build_server_list_msg(dbc.get()));
            clients->sendSMMSettings();
        }
        tick_counter++;
    }

    /* Explicit shutdown ordering: stop accepting before disconnecting
     * clients; join all recv threads (via clients destructor) before
     * draining the write queue; release the DB connection last since the
     * sink captures it. */
    listen.reset();
    clients.reset();
    writer->stop();
    writer.reset();
    dbc.reset();
}
