#include "server-clients.hpp"
#include "fss-log.hpp"

#include <algorithm>
#include <utility>

server_clients::~server_clients()
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
        /* Claims die with the client list, so a straggling identify on a
         * recv thread not yet joined below sees a consistently empty map
         * rather than owners that are no longer in `clients` (which
         * resolveDuplicateIdentity would report as an invariant breach). */
        this->asset_owners.clear();
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

void server_clients::cleanupRemovableClients()
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
}

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
            this->clients.push_back(std::move(client));
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
    std::scoped_lock guard(this->lock);
    if (this->shutting_down)
        return;
    /* Release any asset-id claim this client holds (todo/44). Scan by
     * owner rather than looking up client->getCachedAssetId(): a claim
     * whose publication never completed (the client timed out or was
     * evicted while its identify was still in flight) must still be
     * released, or the asset_id would stay unclaimable until restart. */
    for (auto owner_it = this->asset_owners.begin(); owner_it != this->asset_owners.end();)
    {
        owner_it = (owner_it->second == client) ? this->asset_owners.erase(owner_it) : std::next(owner_it);
    }
    auto it = std::find_if(this->clients.begin(), this->clients.end(),
                           [client](const auto &c) -> auto { return c.get() == client; });
    if (it != this->clients.end())
    {
        this->disconnected.push(*it);
        this->clients.erase(it);
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

/* todo/31, reworked for todo/44: called from the identify path once
 * asset_id is resolved, before `newcomer` is marked identified. The
 * duplicate check and the claim are one critical section over
 * asset_owners: the first claimant records itself under `lock`, so a
 * second connection identifying the same asset_id serialises here and
 * sees the claim even though the winner has not yet published the id
 * into its cached_asset_id. (The previous implementation checked a
 * snapshot of *published* ids and left publication to the caller; two
 * concurrent identifies could each miss the other's unpublished claim
 * and both proceed — the todo/44 race.) Because claims are unique per
 * asset_id, evict_oldest has exactly one owner to dethrone.
 *
 * The evictee's disconnect()+clientDisconnected() runs outside `lock`,
 * same as disconnectRevokedClients()/checkTimeouts() elsewhere in this
 * class: clientDisconnected() takes `lock` itself and the shared_ptr
 * grabbed under the lock keeps the evictee alive across the call —
 * disconnect() blocks on socket I/O and joins the recv thread, which
 * must never happen while holding `lock`. */
auto server_clients::resolveDuplicateIdentity(flight_safety_system::server::fss_client *newcomer, uint64_t asset_id)
    -> bool
{
    std::shared_ptr<flight_safety_system::server::fss_client> evictee{};
    bool reject = false;
    bool stale_owner = false;
    {
        std::scoped_lock guard(this->lock);
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
        evictee->disconnect();
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
            client->disconnect();
            this->clientDisconnected(client.get());
            disconnected_count++;
        }
    }
    return disconnected_count;
}

/* todo/34: unconditional version of disconnectRevokedClients above, for
 * the main loop's sustained-DB-write-failure guard — every currently
 * live session gets severed (Tier-3 Path M m05 expects this to trigger
 * aircraft comms-loss RTL) rather than the server silently continuing
 * to accept telemetry it cannot store.
 *
 * Same disconnect()+clientDisconnected() pattern as
 * disconnectRevokedClients() above, so it is safe by the same reasoning:
 * both calls are idempotent (fss_client::disconnect() documents a second
 * call as a safe no-op; clientDisconnected() only acts if the client is
 * still in `clients`, erasing it on the first call), so a client racing
 * its own concurrent teardown mid-snapshot is handled harmlessly rather
 * than double-freed or double-erased. */
auto server_clients::disconnectAll() -> std::size_t
{
    std::size_t disconnected_count = 0;
    for (const auto &client : this->snapshotClients())
    {
        client->disconnect();
        this->clientDisconnected(client.get());
        disconnected_count++;
    }
    return disconnected_count;
}
