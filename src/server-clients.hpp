#pragma once

#include "fss-server.hpp"
#include "fss-log.hpp"

#include <algorithm>
#include <atomic>
#include <list>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

class server_clients : public flight_safety_system::server::fss_client_handler {
private:
    mutable std::mutex lock{};
    std::list<std::shared_ptr<flight_safety_system::server::fss_client>> clients{};
    std::queue<std::shared_ptr<flight_safety_system::server::fss_client>> disconnected{};
    uint32_t total_clients{0};
    std::atomic<bool> shutting_down{false};
    uint64_t client_timeout_ms{30000};
    uint64_t identify_timeout_ms{30000};
    uint64_t rate_capacity{100};
    uint64_t rate_refill_per_s{20};
    /* Guarded by lock. Built by the command poller thread (the only place
     * allowed to read the DB for it); broadcast by the main loop, which must
     * never perform a synchronous DB read. */
    std::shared_ptr<flight_safety_system::transport::fss_message_server_list> cached_server_list{};
    /* Copy the client list under the lock so the caller can act on it
     * outside the lock: sends block on sockets and disconnect() joins recv
     * threads, and neither may ever run while holding it.  A client that is
     * disconnected mid-iteration stays alive via the snapshot's shared_ptr
     * and the operation fails harmlessly; one connected mid-iteration is
     * covered by the caller's next pass. */
    auto snapshotClients() -> std::vector<std::shared_ptr<flight_safety_system::server::fss_client>>
    {
        std::scoped_lock guard(this->lock);
        return {this->clients.begin(), this->clients.end()};
    }
public:
    server_clients() = default;
    ~server_clients() override
    {
        /* Same hazard as cleanupRemovableClients() (see the comment there):
         * disconnect() joins the connection's recv thread, and that thread may
         * itself be blocked acquiring this->lock in clientDisconnected() or
         * broadcastMsg().  Joining while holding the lock would deadlock, so
         * drain both lists under the lock and disconnect outside it.
         * shutting_down is set first so a recv thread that wins the race to
         * clientDisconnected() no-ops instead of re-queueing into a dying
         * object. */
        this->shutting_down = true;
        std::list<std::shared_ptr<flight_safety_system::server::fss_client>> doomed;
        {
            std::scoped_lock guard(this->lock);
            doomed.splice(doomed.end(), this->clients);
            while (!this->disconnected.empty())
            {
                doomed.push_back(std::move(this->disconnected.front()));
                this->disconnected.pop();
            }
        }
        for (const auto &c : doomed)
        {
            c->disconnect();
        }
    }
    server_clients(server_clients &) = delete;
    server_clients(server_clients &&) = delete;
    auto operator=(server_clients &) -> server_clients & = delete;
    auto operator=(server_clients &&) -> server_clients & = delete;
    auto getTotalClients() const -> uint32_t
    {
        std::scoped_lock guard(this->lock);
        return this->total_clients;
    }
    void cleanupRemovableClients()
    {
        /* Drain the disconnected queue under the lock, then call disconnect()
         * outside the lock.  This avoids a deadlock: the recv thread's
         * processMessage can call clientDisconnected / broadcastMsg, both of
         * which also take this->lock.  If we held this->lock while joining the
         * recv thread, and that thread was blocked waiting for this->lock, we
         * would deadlock.  By releasing the lock before joining we break the
         * cycle.
         *
         * disconnect() joins the recv thread while the fss_client object is
         * still fully alive (the shared_ptr keeps it alive until removable goes
         * out of scope), so processMessage can never execute after any member
         * of fss_client has been destroyed. */
        std::vector<std::shared_ptr<flight_safety_system::server::fss_client>> removable;
        {
            std::scoped_lock guard(this->lock);
            while (!this->disconnected.empty())
            {
                removable.push_back(std::move(this->disconnected.front()));
                this->disconnected.pop();
                total_clients--;
            }
        }
        for (auto &client : removable)
        {
            client->disconnect();
        }
        /* removable goes out of scope here; clients are destroyed with the
         * recv thread already stopped. */
    };
    void setClientTimeoutMs(uint64_t ms) { this->client_timeout_ms = ms; }
    void setClientIdentifyTimeoutMs(uint64_t ms) { this->identify_timeout_ms = ms; }
    void setClientRateLimits(uint64_t capacity, uint64_t refill_per_s)
    {
        this->rate_capacity = capacity;
        this->rate_refill_per_s = refill_per_s;
    }
    void clientConnected(std::shared_ptr<flight_safety_system::server::fss_client> client)
    {
        /* Apply config before wiring the connection's message handler: the
         * recv thread is already live, so activating the handler first would
         * let a message reach the client (touching msg_rate) while the rate
         * limiter is still being reconfigured. */
        client->setTimeoutMs(this->client_timeout_ms);
        client->setIdentifyTimeoutMs(this->identify_timeout_ms);
        client->setRateLimits(this->rate_capacity, this->rate_refill_per_s);
        auto *raw = client.get();
        {
            std::scoped_lock guard(this->lock);
            this->total_clients++;
            this->clients.push_back(std::move(client));
        }
        /* Register only after the client is in the list, so a queued message
         * flushed by activate() that triggers clientDisconnected can find it. */
        raw->activate();
    };
    void clientDisconnected(flight_safety_system::server::fss_client *client) override
    {
        std::scoped_lock guard(this->lock);
        if (this->shutting_down)
            return;
        auto it = std::find_if(this->clients.begin(), this->clients.end(),
                               [client](const auto &c) -> auto { return c.get() == client; });
        if (it != this->clients.end())
        {
            this->disconnected.push(*it);
            this->clients.erase(it);
        }
    };
    void broadcastMsg(const std::shared_ptr<flight_safety_system::transport::fss_message> &msg,
                      flight_safety_system::server::fss_client *except = nullptr) override
    {
        for (const auto &client : this->snapshotClients())
        {
            if (client->isAircraft() && client.get() != except)
            {
                client->sendMsg(msg);
            }
        }
    }
    void checkTimeouts()
    {
        for (const auto &client : this->snapshotClients())
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
        /* Sends cached settings only; the poller refreshes the caches. */
        for (const auto &client : this->snapshotClients())
        {
            client->sendSMMSettings();
        }
    };
    /* Synchronous DB reads; poller thread only — see the main-loop DB
     * contract in server.cpp. */
    void refreshSmmSettings()
    {
        for (const auto &client : this->snapshotClients())
        {
            client->refreshSmmSettings();
        }
    };
    void setCachedServerList(std::shared_ptr<flight_safety_system::transport::fss_message_server_list> msg)
    {
        std::scoped_lock guard(this->lock);
        this->cached_server_list = std::move(msg);
    };
    auto getCachedServerList() -> std::shared_ptr<flight_safety_system::transport::fss_message_server_list>
    {
        std::scoped_lock guard(this->lock);
        return this->cached_server_list;
    };
    void sendRTTRequest(const std::shared_ptr<flight_safety_system::transport::fss_message_rtt_request> &rtt_req)
    {
        for (const auto &client : this->snapshotClients())
        {
            client->sendRTTRequest(rtt_req);
        }
    };
    void pollCommands(flight_safety_system::server::IDatabase *dbc)
    {
        for (const auto &client : this->snapshotClients())
        {
            uint64_t asset_id = client->getCachedAssetId(); /* atomic */
            if (asset_id != 0)
            {
                client->setPendingCommand(dbc->getCommand(asset_id));
            }
        }
    };
    void sendCommand()
    {
        for (const auto &client : this->snapshotClients())
        {
            client->sendCommand();
        }
    };
    auto disconnectRevokedClients(const std::string &crl_file) -> std::size_t
    {
        std::size_t disconnected_count = 0;
        for (const auto &client : this->snapshotClients())
        {
            auto conn = client->getConnection();
            if (conn && conn->isPeerCertRevoked(crl_file))
            {
                FSS_LOG_WARN("server", "Disconnecting client with revoked certificate after CRL reload");
                client->disconnect();
                this->clientDisconnected(client.get());
                disconnected_count++;
            }
        }
        return disconnected_count;
    };
};
