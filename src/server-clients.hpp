#pragma once

#include "fss-server.hpp"

#include <atomic>
#include <list>
#include <map>
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
    uint64_t position_staleness_ms{flight_safety_system::server::default_position_staleness_ms};
    uint64_t rate_capacity{100};
    uint64_t rate_refill_per_s{20};
    flight_safety_system::server::duplicate_identity_policy duplicate_identity_policy_{
        flight_safety_system::server::duplicate_identity_reject_newcomer};
    /* todo/45+47: while the DB fail-safe is degraded, new sessions are
     * refused at clientConnected() so aircraft get one latched comms-loss
     * event instead of reconnecting straight back into a server that cannot
     * durably record telemetry or command state. Guarded by lock — the gate
     * check shares the critical section that admits into `clients`, so with
     * the main loop setting the gate *before* disconnectAll(), a concurrent
     * connection either lands in the list before the gate is up (and is
     * severed by the snapshot) or is refused; there is no interleaving that
     * slips a live session past the fail-safe. */
    bool degraded_{false};
    /* Which live client currently holds each asset_id (todo/44). Guarded by
     * lock. A claim is recorded in resolveDuplicateIdentity() atomically with
     * the duplicate check — i.e. before the winning client has published the
     * id into its cached_asset_id — and released in clientDisconnected(), so
     * two connections identifying the same asset_id at the same instant
     * serialise on the claim and at most one live session can ever hold a
     * given asset_id. */
    std::map<uint64_t, flight_safety_system::server::fss_client *> asset_owners{};
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
    ~server_clients() override;
    server_clients(server_clients &) = delete;
    server_clients(server_clients &&) = delete;
    auto operator=(server_clients &) -> server_clients & = delete;
    auto operator=(server_clients &&) -> server_clients & = delete;
    auto getTotalClients() const -> uint32_t
    {
        std::scoped_lock guard(this->lock);
        return this->total_clients;
    }
    void cleanupRemovableClients();
    void setClientTimeoutMs(uint64_t ms) { this->client_timeout_ms = ms; }
    void setClientIdentifyTimeoutMs(uint64_t ms) { this->identify_timeout_ms = ms; }
    void setClientStalenessMs(uint64_t ms) { this->position_staleness_ms = ms; }
    void setClientRateLimits(uint64_t capacity, uint64_t refill_per_s)
    {
        this->rate_capacity = capacity;
        this->rate_refill_per_s = refill_per_s;
    }
    void setDuplicateIdentityPolicy(flight_safety_system::server::duplicate_identity_policy policy)
    {
        this->duplicate_identity_policy_ = policy;
    }
    /* todo/45+47: raise/lower the fail-safe admission gate (see degraded_
     * above). The main loop sets true immediately before disconnectAll() on a
     * fail-safe trip, and false once the db_failsafe monitor reports
     * recovery. */
    void setDegraded(bool degraded)
    {
        std::scoped_lock guard(this->lock);
        this->degraded_ = degraded;
    }
    void clientConnected(std::shared_ptr<flight_safety_system::server::fss_client> client);
    void clientDisconnected(flight_safety_system::server::fss_client *client) override;
    void broadcastMsg(const std::shared_ptr<flight_safety_system::transport::fss_message> &msg,
                      flight_safety_system::server::fss_client *except = nullptr) override;
    void checkTimeouts();
    void sendSMMSettings()
    {
        /* Sends cached settings only; the poller refreshes the caches. Scheduled
         * on each client's outbound writer thread (todo/21) so a stalled peer
         * cannot hold up the periodic config push to the others. */
        for (const auto &client : this->snapshotClients())
        {
            client->queueSMMSettings();
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
    void sendRTTRequest()
    {
        /* Schedule on each client's outbound writer thread (todo/21). Each client
         * builds its own request, so there is no shared rtt_request instance to
         * race the per-connection id stamp, and a stalled peer cannot hold up RTT
         * scheduling for the others. */
        for (const auto &client : this->snapshotClients())
        {
            client->queueRTTRequest();
        }
    };
    void pollCommands(flight_safety_system::server::IDatabase *dbc)
    {
        /* Capture (client, asset_id) once rather than re-reading the atomic
         * cached_asset_id when mapping results back: a client that identifies
         * mid-pass must not be queried under one id and looked up under
         * another. */
        std::vector<std::pair<std::shared_ptr<flight_safety_system::server::fss_client>, uint64_t>> identified;
        std::vector<uint64_t> asset_ids;
        for (const auto &client : this->snapshotClients())
        {
            uint64_t asset_id = client->getCachedAssetId(); /* atomic */
            if (asset_id != 0)
            {
                identified.emplace_back(client, asset_id);
                asset_ids.push_back(asset_id);
            }
        }
        if (identified.empty())
        {
            return;
        }
        /* One query for the whole fleet's newest-command-per-asset instead of a
         * getCommand round-trip per client (todo/23): 10*N reads/sec collapse to
         * 10/sec. An asset absent from the map has no pending command -- the same
         * as getCommand returning an engaged nullptr -- so clear it, preserving
         * the prior per-client behaviour exactly (the resend-window dedup in
         * sendCommand still governs what actually reaches the aircraft). */
        auto commands = dbc->getCommands(asset_ids);
        /* nullopt is the read failing, in which case "absent from the map" means
         * "not read", not "no command". Leave every client's pending command as
         * it stands and retry on the next 100 ms tick rather than clearing the
         * fleet's commands on the strength of a failed read (todo/69). */
        if (!commands.has_value())
        {
            return;
        }
        for (const auto &[client, asset_id] : identified)
        {
            auto found = commands->find(asset_id);
            client->setPendingCommand(found != commands->end() ? found->second : nullptr);
        }
    };
    void sendCommand()
    {
        /* Schedule on each client's outbound writer thread rather than sending
         * inline (todo/21): a peer with a black-holed socket can then only stall
         * its own writer, never command dispatch for the other clients. */
        for (const auto &client : this->snapshotClients())
        {
            client->queueCommandSend();
        }
    };
    auto resolveDuplicateIdentity(flight_safety_system::server::fss_client *newcomer, uint64_t asset_id)
        -> bool override;
    auto disconnectRevokedClients(const std::string &crl_file) -> std::size_t;
    auto disconnectAll() -> std::size_t;
};
