#pragma once

#include "fss-server.hpp"
#include "fss-log.hpp"

#include <algorithm>
#include <atomic>
#include <list>
#include <mutex>
#include <queue>
#include <vector>

class server_clients : public flight_safety_system::server::fss_client_handler {
private:
    std::mutex lock{};
    std::list<std::shared_ptr<flight_safety_system::server::fss_client>> clients{};
    std::queue<std::shared_ptr<flight_safety_system::server::fss_client>> disconnected{};
    uint32_t total_clients{0};
    std::atomic<bool> shutting_down{false};
    uint64_t client_timeout_ms{30000};
    uint64_t rate_capacity{100};
    uint64_t rate_refill_per_s{20};
public:
    server_clients() = default;
    ~server_clients() override {
        this->shutting_down = true;
        std::scoped_lock guard(this->lock);
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
        std::scoped_lock guard(this->lock);
        while(!this->disconnected.empty())
        {
            auto client = this->disconnected.front();
            this->disconnected.pop();
            total_clients--;
        }
    };
    void setClientTimeoutMs(uint64_t ms) { this->client_timeout_ms = ms; }
    void setClientRateLimits(uint64_t capacity, uint64_t refill_per_s)
    {
        this->rate_capacity = capacity;
        this->rate_refill_per_s = refill_per_s;
    }
    void clientConnected(std::shared_ptr<flight_safety_system::server::fss_client> client)
    {
        client->setTimeoutMs(this->client_timeout_ms);
        client->setRateLimits(this->rate_capacity, this->rate_refill_per_s);
        std::scoped_lock guard(this->lock);
        this->total_clients++;
        this->clients.push_back(std::move(client));
    };
    void clientDisconnected(flight_safety_system::server::fss_client *client) override
    {
        std::scoped_lock guard(this->lock);
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
        std::scoped_lock guard(this->lock);
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
            std::scoped_lock guard(this->lock);
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
        std::scoped_lock guard(this->lock);
        for(const auto &client: this->clients)
        {
            client->sendSMMSettings();
        }
    };
    void sendRTTRequest(const std::shared_ptr<flight_safety_system::transport::fss_message_rtt_request> &rtt_req)
    {
        std::vector<std::shared_ptr<flight_safety_system::server::fss_client>> snapshot;
        {
            std::scoped_lock guard(this->lock);
            std::copy(this->clients.begin(), this->clients.end(), std::back_inserter(snapshot));
        }
        for (const auto &client : snapshot)
        {
            client->sendRTTRequest(rtt_req);
        }
    };
    void pollCommands(flight_safety_system::server::IDatabase *dbc)
    {
        std::vector<std::pair<std::shared_ptr<flight_safety_system::server::fss_client>, uint64_t>> snapshot;
        {
            std::scoped_lock guard(this->lock);
            for (const auto &c : this->clients)
            {
                uint64_t id = c->getCachedAssetId();
                if (id != 0) { snapshot.emplace_back(c, id); }
            }
        }
        for (auto &[client, asset_id] : snapshot)
        {
            client->setPendingCommand(dbc->getCommand(asset_id));
        }
    };
    void sendCommand()
    {
        std::scoped_lock guard(this->lock);
        for(const auto &client: this->clients)
        {
            client->sendCommand();
        }
    };
    void disconnectRevokedClients(const std::string &crl_file)
    {
        std::vector<std::shared_ptr<flight_safety_system::server::fss_client>> snapshot;
        {
            std::scoped_lock guard(this->lock);
            std::copy(this->clients.begin(), this->clients.end(), std::back_inserter(snapshot));
        }
        for (const auto &client : snapshot)
        {
            auto conn = client->getConnection();
            if (conn && conn->isPeerCertRevoked(crl_file))
            {
                FSS_LOG_WARN("server", "Disconnecting client with revoked certificate after CRL reload");
                client->disconnect();
                this->clientDisconnected(client.get());
            }
        }
    };
};
