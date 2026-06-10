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
        /* Snapshot, then send outside the lock (as sendRTTRequest does):
         * sends block on the socket, and holding the global lock across a
         * blocking send lets one stuck client stall every recv thread that
         * needs clientDisconnected()/broadcastMsg() — and the main loop.
         * A client disconnected mid-iteration is harmless: the snapshot's
         * shared_ptr keeps it alive and the send just fails. */
        std::vector<std::shared_ptr<flight_safety_system::server::fss_client>> snapshot;
        {
            std::scoped_lock guard(this->lock);
            std::copy(this->clients.begin(), this->clients.end(), std::back_inserter(snapshot));
        }
        for (const auto &client : snapshot)
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
        /* Snapshot, then act outside the lock — see broadcastMsg(). */
        std::vector<std::shared_ptr<flight_safety_system::server::fss_client>> snapshot;
        {
            std::scoped_lock guard(this->lock);
            std::copy(this->clients.begin(), this->clients.end(), std::back_inserter(snapshot));
        }
        for (const auto &client : snapshot)
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
                if (id != 0)
                {
                    snapshot.emplace_back(c, id);
                }
            }
        }
        for (auto &[client, asset_id] : snapshot)
        {
            client->setPendingCommand(dbc->getCommand(asset_id));
        }
    };
    void sendCommand()
    {
        /* Snapshot, then act outside the lock — see broadcastMsg(). */
        std::vector<std::shared_ptr<flight_safety_system::server::fss_client>> snapshot;
        {
            std::scoped_lock guard(this->lock);
            std::copy(this->clients.begin(), this->clients.end(), std::back_inserter(snapshot));
        }
        for (const auto &client : snapshot)
        {
            client->sendCommand();
        }
    };
    auto disconnectRevokedClients(const std::string &crl_file) -> std::size_t
    {
        std::vector<std::shared_ptr<flight_safety_system::server::fss_client>> snapshot;
        {
            std::scoped_lock guard(this->lock);
            std::copy(this->clients.begin(), this->clients.end(), std::back_inserter(snapshot));
        }
        std::size_t disconnected_count = 0;
        for (const auto &client : snapshot)
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
