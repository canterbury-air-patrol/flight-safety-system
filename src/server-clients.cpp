#include "server-clients.hpp"
#include "fss-log.hpp"

#include <algorithm>
#include <utility>

server_clients::server_clients() : cleanup_worker(&server_clients::runCleanup, this) {}

void server_clients::runCleanup()
{
    for (;;)
    {
        std::shared_ptr<flight_safety_system::server::fss_client> client;
        {
            std::unique_lock guard(this->cleanup_lock);
            this->cleanup_cv.wait(guard,
                                  [this]() -> bool { return this->cleanup_stopping || !this->cleanup_queue.empty(); });
            if (this->cleanup_queue.empty())
            {
                return;
            }
            client = std::move(this->cleanup_queue.front());
            this->cleanup_queue.pop();
        }
        client->disconnect();
    }
}

server_clients::~server_clients()
{
    this->shutting_down = true;
    std::list<std::shared_ptr<flight_safety_system::server::fss_client>> doomed;
    {
        std::scoped_lock guard(this->lock);
        this->asset_owners.clear();
        doomed.splice(doomed.end(), this->clients);
        while (!this->disconnected.empty())
        {
            doomed.push_back(std::move(this->disconnected.front()));
            this->disconnected.pop();
        }
    }
    // Sever every socket before waiting for even one database-backed callback.
    for (const auto &client : doomed)
    {
        client->requestDisconnect();
    }
    {
        std::scoped_lock guard(this->cleanup_lock);
        for (auto &client : doomed)
        {
            this->cleanup_queue.push(std::move(client));
        }
        this->cleanup_stopping = true;
    }
    this->cleanup_cv.notify_one();
    this->cleanup_worker.join();
}

void server_clients::cleanupRemovableClients()
{
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
    {
        std::scoped_lock guard(this->cleanup_lock);
        for (auto &client : removable)
        {
            this->cleanup_queue.push(std::move(client));
        }
    }
    this->cleanup_cv.notify_one();
}

/* Own the session across activate(): cleanup may remove the list's reference
 * while activation is still flushing a callback. */
// NOLINTNEXTLINE(performance-unnecessary-value-param)
void server_clients::clientConnected(std::shared_ptr<flight_safety_system::server::fss_client> client)
{
    /* Apply config before wiring the connection's message handler: the
     * recv thread is already live, so activating the handler first would
     * let a message reach the client (touching msg_rate) while the rate
     * limiter is still being reconfigured. */
    client->setTimeoutMs(this->client_timeout_ms);
    client->setIdentifyTimeoutMs(this->identify_timeout_ms);
    client->setStalenessMs(this->position_staleness_ms);
    client->setRateLimits(this->rate_capacity, this->rate_refill_per_s);
    auto *raw = client.get();
    bool refused = false;
    {
        std::scoped_lock guard(this->lock);
        refused = this->degraded_;
        if (!refused)
        {
            this->total_clients++;
            this->clients.push_back(client);
        }
    }
    if (refused)
    {
        /* Sever outside the lock (disconnect() joins the recv thread —
         * see cleanupRemovableClients). The client was never admitted or
         * activated, so nothing else references it and it dies with
         * `client` when this frame returns. */
        FSS_LOG_WARN("server", "Refusing new client: DB fail-safe is degraded; "
                               "admission resumes once the recovery condition clears");
        raw->disconnect();
        return;
    }
    /* Register only after the client is in the list, so a queued message
     * flushed by activate() that triggers clientDisconnected can find it. */
    raw->activate();
}

void server_clients::clientDisconnected(flight_safety_system::server::fss_client *client)
{
    std::shared_ptr<flight_safety_system::server::fss_client> removed;
    {
        std::scoped_lock guard(this->lock);
        if (this->shutting_down)
        {
            return;
        }
        for (auto owner_it = this->asset_owners.begin(); owner_it != this->asset_owners.end();)
        {
            owner_it = (owner_it->second == client) ? this->asset_owners.erase(owner_it) : std::next(owner_it);
        }
        auto it = std::find_if(this->clients.begin(), this->clients.end(),
                               [client](const auto &c) -> bool { return c.get() == client; });
        if (it != this->clients.end())
        {
            removed = *it;
            this->disconnected.push(removed);
            this->clients.erase(it);
        }
    }
    if (removed != nullptr)
    {
        removed->requestDisconnect();
    }
}

/* todo/36: broadcasts are scheduled on each client's outbound writer
 * thread rather than sent inline, so a black-holed peer can only stall its
 * own broadcast delivery — never the main loop (server-list, every 15s)
 * or another client's recv thread (position relay, called from the
 * *reporting* client's recv thread). fss_connection::sendMsg stamps a
 * per-connection id into the message instance it is given (the C8
 * invariant), so each client must get its own instance: decode() over
 * getPacked() builds an independent clone per client rather than sharing
 * `msg` (packed once, since the bytes don't depend on the recipient).
 *
 * todo/59: the isAircraft() filter below is deliberate, not an oversight —
 * non-aircraft clients (ADS-B feeders, config utilities) are telemetry
 * producers and never consumers of relayed traffic, so broadcasts are
 * aircraft-only by design. That also means the periodic 15 s server-list
 * push intentionally skips them: they get their server list from their
 * own config, not from discovery. */
void server_clients::broadcastMsg(const std::shared_ptr<flight_safety_system::transport::fss_message> &msg,
                                  flight_safety_system::server::fss_client *except)
{
    bool is_server_list = msg->getType() == flight_safety_system::transport::message_type_server_list;
    auto packed = msg->getPacked();
    /* todo/55: pack once and share the frame, read-only, across every recipient
     * — each stamps only its own id into a private copy at send time
     * (fss_connection::sendPacked), so there is no per-recipient decode or
     * re-pack. We also drop the old once-per-broadcast validation decode:
     * getPacked() already framed the message and updateSize() invalidates a
     * frame too large to frame, so isValid() is the cheap equivalent of the old
     * "did it round-trip" guard. Invalidity is a property of `packed`, not of any
     * one recipient, so drop the whole broadcast and log once rather than
     * silently per client. */
    if (!packed->isValid())
    {
        FSS_LOG_ERROR("server", "broadcastMsg: message of type " << msg->getType()
                                                                 << " did not pack into a valid frame; dropping");
        return;
    }
    for (const auto &client : this->snapshotClients())
    {
        if (client->isAircraft() && client.get() != except)
        {
            if (is_server_list)
            {
                client->queueServerListBroadcast(packed);
            }
            else
            {
                client->queuePositionRelay(packed);
            }
        }
    }
}

void server_clients::checkTimeouts()
{
    for (const auto &client : this->snapshotClients())
    {
        if (client->isTimedOut())
        {
            FSS_LOG_WARN("server", "Client timed out, disconnecting");
            this->clientDisconnected(client.get());
        }
    }
}

/* Reserve identity under lock before the caller publishes cached_asset_id.
 * Eviction only shuts down the old socket; the cleanup worker joins it later.
 * Neither the receive thread identifying the newcomer nor the main loop may
 * wait for the old session's DB-backed callback. */
auto server_clients::resolveDuplicateIdentity(flight_safety_system::server::fss_client *newcomer, uint64_t asset_id)
    -> bool
{
    std::shared_ptr<flight_safety_system::server::fss_client> evictee{};
    bool reject = false;
    bool stale_owner = false;
    {
        std::scoped_lock guard(this->lock);
        /* A DB lookup can return after timeout removal. Such a session must
         * never reclaim an identity while its deferred teardown is pending. */
        if (this->shutting_down || std::none_of(this->clients.begin(), this->clients.end(),
                                                [newcomer](const auto &c) -> bool { return c.get() == newcomer; }))
        {
            return false;
        }
        auto owner = this->asset_owners.find(asset_id);
        if (owner != this->asset_owners.end() && owner->second != newcomer)
        {
            /* Invariant: a recorded owner is always still in `clients`.
             * clientDisconnected() erases the claim in the same critical
             * section that removes the client, and ~server_clients clears
             * the map alongside the list, so a miss here means some
             * removal path failed to release its claim. That breach must
             * not stay silent: under reject_newcomer the asset would be
             * refused forever; under evict_oldest the "eviction" severs
             * nobody while resolve still reports success. Behaviour is
             * unchanged either way (reject still rejects; evict_oldest
             * transfers the claim with nobody to disconnect) — the ERROR
             * below (logged outside the lock, like every other nontrivial
             * action in this class) is what makes the breach visible. */
            auto it = std::find_if(this->clients.begin(), this->clients.end(),
                                   [&owner](const auto &c) -> bool { return c.get() == owner->second; });
            stale_owner = it == this->clients.end();
            if (this->duplicate_identity_policy_ == flight_safety_system::server::duplicate_identity_reject_newcomer)
            {
                reject = true;
            }
            else
            {
                if (!stale_owner)
                {
                    evictee = *it;
                }
                this->asset_owners[asset_id] = newcomer;
            }
        }
        else
        {
            this->asset_owners[asset_id] = newcomer;
        }
    }
    if (stale_owner)
    {
        FSS_LOG_ERROR("server", "Duplicate identity for asset_id "
                                    << asset_id
                                    << ": recorded claim owner is not in the live client list — a client-removal "
                                       "path failed to release its claim (invariant breach)");
    }
    if (reject)
    {
        return false;
    }
    if (evictee != nullptr)
    {
        this->clientDisconnected(evictee.get());
        FSS_LOG_WARN("server", "Duplicate identity for asset_id "
                                   << asset_id << ": evicted the existing session (duplicate_identity_evict_oldest)");
    }
    return true;
}

auto server_clients::disconnectRevokedClients(const std::string &crl_file) -> std::size_t
{
    std::size_t disconnected_count = 0;
    for (const auto &client : this->snapshotClients())
    {
        auto conn = client->getConnection();
        if (conn && conn->isPeerCertRevoked(crl_file))
        {
            FSS_LOG_WARN("server", "Disconnecting client with revoked certificate after CRL reload");
            this->clientDisconnected(client.get());
            disconnected_count++;
        }
    }
    return disconnected_count;
}

/* The poller only detects retirement. The main loop removes these sessions
 * and shuts down their sockets; the cleanup worker owns blocking teardown.
 * Removing the session also releases its asset claim for reactivation. */
auto server_clients::disconnectRetiredClients() -> std::size_t
{
    std::vector<std::shared_ptr<flight_safety_system::server::fss_client>> retired{};
    {
        std::scoped_lock guard(this->lock);
        retired.swap(this->pending_retirement);
    }
    std::size_t disconnected_count = 0;
    for (const auto &client : retired)
    {
        /* Logged per session rather than as a count: retirement is an
         * administrative act on one aircraft, and an operator who retires the
         * wrong asset needs to see which session went. */
        FSS_LOG_WARN("server", "Disconnecting session for retired asset_id " << client->getCachedAssetId()
                                                                             << " (fss-web retired_at is set)");
        this->clientDisconnected(client.get());
        disconnected_count++;
    }
    return disconnected_count;
}

/* The fail-safe closes every socket promptly even if an earlier session's
 * receive callback is stuck in the database. Joins happen on the cleanup worker. */
auto server_clients::disconnectAll() -> std::size_t
{
    std::size_t disconnected_count = 0;
    for (const auto &client : this->snapshotClients())
    {
        this->clientDisconnected(client.get());
        disconnected_count++;
    }
    return disconnected_count;
}
