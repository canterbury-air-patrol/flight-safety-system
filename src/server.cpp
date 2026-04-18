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
public:
    server_clients() = default;
    ~server_clients() {
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
    void clientConnected(std::shared_ptr<flight_safety_system::server::fss_client> client)
    {
        std::lock_guard<std::mutex> guard(this->lock);
        this->total_clients++;
        this->clients.push_back(std::move(client));
    };
    void clientDisconnected(flight_safety_system::server::fss_client *client) override
    {
        std::lock_guard<std::mutex> guard(this->lock);
        if (this->shutting_down) return;
        auto it = std::find_if(this->clients.begin(), this->clients.end(), [client](const auto &c) {
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

    auto clients = std::make_shared<server_clients>();

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
    listen = std::make_shared<flight_safety_system::transport_ssl::fss_listen>(config["port"].asInt(),
        [dbc, &clients](std::shared_ptr<flight_safety_system::transport::fss_connection> conn) -> bool {
#ifdef DEBUG
            std::cout << "New client connected" << std::endl;
#endif
            clients->clientConnected(std::make_shared<flight_safety_system::server::fss_client>(std::move(conn), dbc.get(), clients.get()));
            return true;
        },
        config["ssl"]["ca_public_key"].asString(), config["ssl"]["server_private_key"].asString(), config["ssl"]["server_public_key"].asString());

    uint64_t counter = 0;
    constexpr int send_config_period = 15;
    while (running == 1)
    {
        sleep (1);
        clients->cleanupRemovableClients();
        {
            auto rtt_req = std::make_shared<flight_safety_system::transport::fss_message_rtt_request>();
            clients->sendRTTRequest(rtt_req);
            clients->sendCommand();
        }
        if ((counter % send_config_period) == 0)
        {
            clients->broadcastMsg(flight_safety_system::server::build_server_list_msg(dbc.get()));
            clients->sendSMMSettings();
        }
        counter++;
    }
}
