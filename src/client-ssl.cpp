#include <fss-client-ssl.hpp>
#include "fss-log.hpp"
#include "json-config.hpp"

#include <algorithm>
#include <iostream>
#include <fstream>
#include <string>
#include <utility>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
#include <json/json.h>
#pragma GCC diagnostic pop

flight_safety_system::client_ssl::fss_client::fss_client(const std::string &t_fileName)
{
    /* Open the config file */
    std::ifstream configfile(t_fileName);
    if (!configfile.is_open())
    {
        FSS_LOG_ERROR("client", "Failed to load configuration");
        return;
    }
    /* jsoncpp throws Json::Exception on malformed JSON or wrong-typed
     * values; this is a library constructor, so contain it and leave the
     * client without a usable server list (like the missing-file path
     * above) rather than letting the exception escape into the consumer. */
    try
    {
        Json::Value config;
        configfile >> config;

        this->setAssetName(config["name"].asString());
        if (config.isMember("non_aircraft"))
        {
            this->setNonAircraft(config["non_aircraft"].asBool());
        }
        if (config.isMember("clock_offset_ms"))
        {
            this->setClockOffsetMs(config["clock_offset_ms"].asInt64());
        }
        if (config.isMember("tcp_user_timeout_ms"))
        {
            this->setTcpUserTimeoutMs(config["tcp_user_timeout_ms"].asUInt());
        }
        if (config.isMember("learned_server_expiry_ms"))
        {
            this->setLearnedServerExpiryMs(config["learned_server_expiry_ms"].asUInt64());
        }
        if (config.isMember("max_learned_servers"))
        {
            this->setMaxLearnedServers(config["max_learned_servers"].asUInt());
        }

        this->ca_file = config["ssl"]["ca_public_key"].asString();
        this->private_key_file = config["ssl"]["client_private_key"].asString();
        this->public_key_file = config["ssl"]["client_public_key"].asString();

        /* Load all the known servers from the config */
        for (unsigned int idx = 0; idx < config["servers"].size(); idx++)
        {
            uint16_t port = 0;
            if (!read_json_tcp_port(config["servers"][idx]["port"], "client",
                                    "server config at index " + std::to_string(idx), port))
            {
                continue;
            }
            auto server = std::make_shared<flight_safety_system::client_ssl::fss_server>(
                this, config["servers"][idx]["address"].asString(), port, this->ca_file, this->private_key_file,
                this->public_key_file);
            this->addServer(server);
        }
    }
    catch (const Json::Exception &e)
    {
        FSS_LOG_ERROR("client", "Invalid configuration in " << t_fileName << ": " << e.what());
    }
}

flight_safety_system::client_ssl::fss_client::fss_client() = default;

flight_safety_system::client_ssl::fss_client::fss_client(std::string t_ca, std::string t_private_key,
                                                         std::string t_public_key)
    : ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)), public_key_file(std::move(t_public_key))
{
}

flight_safety_system::client_ssl::fss_client::~fss_client()
{
    // Drain both lists under the lock so that any concurrent
    // serverRequiresReconnect call sees empty lists and cannot push_back
    // into a list that is already being destroyed.  Disconnect outside the
    // lock: server->disconnect() joins the recv thread, which can call
    // serverRequiresReconnect (which tries to acquire servers_lock).
    std::list<std::shared_ptr<fss_server>> all;
    {
        std::scoped_lock lock(this->servers_lock);
        all.splice(all.end(), this->servers);
        all.splice(all.end(), this->reconnect_servers);
        /* Anything expired but not yet drained by attemptReconnect()
         * (docs/decisions/66-67-client-outbound-fanout.md) still owns a worker
         * thread and possibly a connection. */
        all.splice(all.end(), this->expired_servers);
    }
    for (const auto &server : all)
    {
        server->disconnect();
    }
}

void flight_safety_system::client_ssl::fss_client::disconnect()
{
    /* Snapshot the list under the lock so that concurrent serverRequiresReconnect
     * calls cannot mutate servers while we iterate.  Disconnect outside the
     * lock: server->disconnect() joins the recv thread, which can call
     * serverRequiresReconnect (which tries to acquire servers_lock). */
    std::list<std::shared_ptr<fss_server>> snapshot;
    {
        std::scoped_lock lock(this->servers_lock);
        snapshot = this->servers;
    }
    for (const auto &server : snapshot)
    {
        server->disconnect();
    }
}

void flight_safety_system::client_ssl::fss_client::updateConfigured()
{
    /* A client is configured once it has an asset name and at least one server
     * (live or pending reconnect). Centralised here so the file ctor, the
     * programmatic setAssetName/connectTo path, and any future mutator all keep
     * isConfigured() in step. Self-locking so no caller has to remember to. */
    std::scoped_lock lock(this->servers_lock);
    this->configured = !this->asset_name.empty() && (!this->servers.empty() || !this->reconnect_servers.empty());
}

void flight_safety_system::client_ssl::fss_client::setAssetName(std::string t_asset_name)
{
    /* asset_name is set once during configuration, before the client is used
     * concurrently, so it is not guarded by servers_lock (getAssetName() also
     * reads it unlocked). Recompute the configured flag afterwards. */
    this->asset_name = std::move(t_asset_name);
    this->updateConfigured();
}

void flight_safety_system::client_ssl::fss_client::setNonAircraft(bool t_non_aircraft)
{
    /* Set once during configuration, before concurrent use (same as
     * setAssetName above) -- no lock needed. */
    this->non_aircraft = t_non_aircraft;
}

void flight_safety_system::client_ssl::fss_client::setClockOffsetMs(int64_t t_offset_ms)
{
    /* Set once during configuration, before concurrent use (same as
     * setAssetName above) -- no lock needed. */
    this->clock_offset_ms = t_offset_ms;
}

void flight_safety_system::client_ssl::fss_client::setTcpUserTimeoutMs(unsigned int t_timeout_ms)
{
    /* Set once during configuration, before concurrent use (same as
     * setAssetName above) -- no lock needed. fss_server::reconnect_to()
     * reads it at every (re)connect, so it also applies to servers learned
     * later from a server-list update, not just config-file entries. */
    this->tcp_user_timeout_ms = t_timeout_ms;
}

void flight_safety_system::client_ssl::fss_client::setLearnedServerExpiryMs(uint64_t t_expiry_ms)
{
    /* Set once during configuration, before concurrent use (same as
     * setAssetName above) -- no lock needed. */
    this->learned_server_expiry_ms = t_expiry_ms;
}

void flight_safety_system::client_ssl::fss_client::setMaxLearnedServers(size_t t_max)
{
    /* Set once during configuration, before concurrent use (same as
     * setAssetName above) -- no lock needed. */
    this->max_learned_servers = t_max;
}

void flight_safety_system::client_ssl::fss_client::setClock(std::shared_ptr<flight_safety_system::IClock> t_clock)
{
    if (t_clock == nullptr)
    {
        return;
    }
    this->clock = std::move(t_clock);
}

auto flight_safety_system::client_ssl::fss_client::getSkewedTimestamp() const -> uint64_t
{
    /* The timestamp is well below 2^63, so the signed round trip cannot
     * overflow for any offset an e2e test would configure. */
    return static_cast<uint64_t>(static_cast<int64_t>(flight_safety_system::fss_current_timestamp()) +
                                 this->clock_offset_ms);
}

void flight_safety_system::client_ssl::fss_client::connectTo(const std::string &t_address, uint16_t t_port,
                                                             bool t_connect)
{
    auto server = std::make_shared<flight_safety_system::client_ssl::fss_server>(
        this, t_address, t_port, this->ca_file, this->private_key_file, this->public_key_file);
    if (t_connect)
    {
        /* This dial is synchronous, on the caller's thread, so it is the one
         * (re)connect that no queueReconnect() has seeded the client snapshot
         * for. Seed it here rather than from the fss_server constructor: the
         * file constructor builds servers while the fss_client is itself still
         * under construction, and reading a virtual off an object under
         * construction would both miss a subclass override and be exactly the
         * ctor-side twin of the teardown race the snapshot exists to remove. */
        server->refreshClientConfig();
        server->reconnect();
    }
    this->addServer(server);
}

void flight_safety_system::client_ssl::fss_client::attemptReconnect()
{
    /* Phase (a): snapshot servers under the lock to find timed-out entries.
     * isServerTimedOut() reads server-local atomics — no list access needed. */
    std::list<std::shared_ptr<fss_server>> servers_snapshot;
    {
        std::scoped_lock lock(this->servers_lock);
        servers_snapshot = this->servers;
    }
    std::list<fss_server *> timed_out;
    for (const auto &server : servers_snapshot)
    {
        if (server->isServerTimedOut())
        {
            timed_out.push_back(server.get());
        }
    }

    /* Phase (b): schedule reconnection for timed-out servers.
     * serverRequiresReconnect() acquires servers_lock internally; we must NOT
     * hold it here to avoid a self-deadlock (plain std::mutex). */
    for (auto *server : timed_out)
    {
        FSS_LOG_WARN("client", "Server connection timed out, scheduling reconnect");
        this->serverRequiresReconnect(server);
    }

    /* Phase (c): snapshot reconnect_servers under the lock, then, outside it,
     * harvest the servers whose queued reconnect has since succeeded and
     * schedule an attempt for the rest.
     *
     * The dial itself runs on each server's own outbound worker (docs/decisions/66-67-client-outbound-fanout.md).
     * Calling the blocking reconnect() here — a connect() plus a TLS
     * handshake, ~7 s for a host that drops SYNs and up to 10 s for one that
     * stalls the handshake — made this loop cost the SUM of every unreachable
     * entry's timeout, delaying reconnection to a healthy server that dropped
     * in the same window. queueReconnect() returns immediately, so the cost of
     * a pass is now the slowest server's timeout in parallel rather than every
     * server's in series, and unreachable entries no longer hold each other up.
     * List mutation stays here, on the thread that owns servers_lock; the
     * worker never reaches back into the client. */
    std::list<std::shared_ptr<fss_server>> reconnect_snapshot;
    {
        std::scoped_lock lock(this->servers_lock);
        reconnect_snapshot = this->reconnect_servers;
    }
    std::list<std::shared_ptr<fss_server>> reconnected;
    bool any_connected = false;
    for (auto const &server : reconnect_snapshot)
    {
        if (server->takeReconnectSucceeded())
        {
            reconnected.push_back(server);
            any_connected = true;
        }
        else
        {
            server->queueReconnect();
        }
    }

    /* Phase (d): move the newly connected servers from reconnect_servers to
     * servers under the lock. Promote only entries still *in* that list: the
     * snapshot above was taken with the lock released, so updateServers() can
     * have expired one in between, and blindly pushing would resurrect a
     * server that phase (e) is about to disconnect. */
    {
        std::scoped_lock lock(this->servers_lock);
        while (!reconnected.empty())
        {
            auto server = reconnected.front();
            reconnected.pop_front();
            auto found = std::find(this->reconnect_servers.begin(), this->reconnect_servers.end(), server);
            if (found == this->reconnect_servers.end())
            {
                continue;
            }
            this->reconnect_servers.erase(found);
            this->servers.push_back(server);
        }
    }

    /* Phase (e): tear down anything updateServers() expired (docs/decisions/66-67-client-outbound-fanout.md). It
     * removed them from both lists but could not disconnect them: it runs on a
     * recv thread, and the expiring server can be the one the list arrived on,
     * so the join would be a self-join. Here we are on the thread that already
     * owns connection lifecycle, and the shared_ptr keeps each object alive
     * until we are done with it. */
    std::list<std::shared_ptr<fss_server>> expired;
    {
        std::scoped_lock lock(this->servers_lock);
        expired.splice(expired.end(), this->expired_servers);
    }
    for (auto const &server : expired)
    {
        server->disconnect();
    }

    /* Phase (f): notify outside the lock — connectionStatusChange() is a
     * virtual user callback that may re-enter the client. */
    if (any_connected || !expired.empty())
    {
        this->notifyConnectionStatus();
    }
}

void flight_safety_system::client_ssl::fss_client::sendMsgAll(
    const std::shared_ptr<flight_safety_system::transport::fss_message> &msg)
{
    if (msg == nullptr)
    {
        return;
    }
    /* Copy the list under the lock so that concurrent serverRequiresReconnect
     * calls cannot invalidate the iterator mid-fan-out. */
    std::list<std::shared_ptr<fss_server>> snapshot;
    {
        std::scoped_lock lock(this->servers_lock);
        snapshot = this->servers;
    }
    if (snapshot.empty())
    {
        return;
    }
    /* Pack ONCE, then hand the same read-only frame to every server's outbound
     * worker (docs/decisions/66-67-client-outbound-fanout.md). This replaced a
     * serial loop of blocking server->sendMsg() calls, in which one server that
     * completed TLS and then stopped reading held up every healthy server
     * behind it for a whole TCP_USER_TIMEOUT.
     *
     * The sends are now concurrent, so the todo/12 C8 invariant (one
     * fss_message instance must not be sent on two connections at once — the
     * id stamp would race) can no longer hold for free. It is re-established
     * exactly as todo/55 did for server-side broadcasts: `packed` is shared as
     * const and never mutated, and each connection copies it and stamps only
     * its own id (fss_connection::sendPacked). */
    std::shared_ptr<const flight_safety_system::transport::buf_len> packed = msg->getPacked();
    for (auto const &server : snapshot)
    {
        server->queueSend(packed);
    }
}

auto flight_safety_system::client_ssl::fss_client::getAssetName() -> std::string
{
    return this->asset_name;
}

auto flight_safety_system::client_ssl::fss_client::isConfigured() const -> bool
{
    std::scoped_lock lock(this->servers_lock);
    return this->configured;
}

auto flight_safety_system::client_ssl::fss_client::getServerCount() const -> size_t
{
    std::scoped_lock lock(this->servers_lock);
    return this->servers.size() + this->reconnect_servers.size();
}

void flight_safety_system::client_ssl::fss_client::addServer(
    const std::shared_ptr<flight_safety_system::client_ssl::fss_server> &server)
{
    /* Read connected() before acquiring the lock: it reads server-local state
     * (whether the server's own connection pointer is set) and does not touch
     * the shared lists. */
    bool is_connected = server->connected();
    {
        std::scoped_lock lock(this->servers_lock);
        if (is_connected)
        {
            this->servers.push_back(server);
        }
        else
        {
            this->reconnect_servers.push_back(server);
        }
    }
    /* updateConfigured() takes servers_lock itself, so recompute after releasing
     * it here rather than holding it across the call. */
    this->updateConfigured();
}

void flight_safety_system::client_ssl::fss_client::addLearnedServer(const std::string &t_address, uint16_t t_port)
{
    auto server = std::make_shared<flight_safety_system::client_ssl::fss_server>(
        this, t_address, t_port, this->ca_file, this->private_key_file, this->public_key_file);
    /* Set before addServer() publishes it into a list: from that moment on
     * these fields belong to servers_lock. */
    server->setLearned(true);
    server->setLastSeenMs(this->clock->now_ms());
    this->addServer(server);
}

static auto find_server(const std::list<std::shared_ptr<flight_safety_system::client_ssl::fss_server>> &servers,
                        const std::string &address, uint16_t port)
    -> std::shared_ptr<flight_safety_system::client_ssl::fss_server>
{
    auto found = std::find_if(servers.begin(), servers.end(), [&address, &port](const auto &n) -> bool {
        return (n->getAddress().compare(address) == 0 && n->getPort() == port);
    });
    return found != servers.end() ? *found : nullptr;
}

/* Remove every learned server last seen before `cutoff_ms` from `from` and
 * return them. Precondition: servers_lock held. */
static auto take_stale_servers(std::list<std::shared_ptr<flight_safety_system::client_ssl::fss_server>> &from,
                               uint64_t cutoff_ms)
    -> std::list<std::shared_ptr<flight_safety_system::client_ssl::fss_server>>
{
    std::list<std::shared_ptr<flight_safety_system::client_ssl::fss_server>> stale;
    for (auto it = from.begin(); it != from.end();)
    {
        if ((*it)->isLearned() && (*it)->getLastSeenMs() < cutoff_ms)
        {
            stale.push_back(std::move(*it));
            it = from.erase(it);
        }
        else
        {
            ++it;
        }
    }
    return stale;
}

/* Count of learned entries in a list. Precondition: servers_lock held. */
static auto count_learned(const std::list<std::shared_ptr<flight_safety_system::client_ssl::fss_server>> &servers)
    -> size_t
{
    return static_cast<size_t>(
        std::count_if(servers.begin(), servers.end(), [](const auto &n) -> bool { return n->isLearned(); }));
}

auto flight_safety_system::client_ssl::fss_client::getLearnedServerCount() const -> size_t
{
    std::scoped_lock lock(this->servers_lock);
    return count_learned(this->servers) + count_learned(this->reconnect_servers);
}

void flight_safety_system::client_ssl::fss_client::updateServers(
    const std::shared_ptr<flight_safety_system::transport::fss_message_server_list> &msg)
{
    /* Before docs/decisions/66-67-client-outbound-fanout.md
     * this method only ever ADDED: there was no removal path
     * anywhere in the file, so a server deactivated in config_serverconfig
     * dropped out of the broadcast list but every client that had ever seen it
     * kept it forever — and kept paying a blocking connect attempt for it every
     * backoff interval, silently. The list is now maintained: an entry present
     * in the broadcast refreshes its last-seen time, an entry absent for the
     * whole expiry window is dropped, and the number that can be learned is
     * capped. Servers from the config file are never expired (see the
     * fss_server::learned member). */
    uint64_t now = this->clock->now_ms();
    std::list<std::pair<std::string, uint16_t>> to_learn;
    size_t learned_count = 0;
    size_t expired_count = 0;
    {
        std::scoped_lock lock(this->servers_lock);
        for (auto const &server_entry : msg->getServers())
        {
            auto known = find_server(this->servers, server_entry.first, server_entry.second);
            if (known == nullptr)
            {
                known = find_server(this->reconnect_servers, server_entry.first, server_entry.second);
            }
            if (known != nullptr)
            {
                known->setLastSeenMs(now);
            }
            else
            {
                to_learn.push_back(server_entry);
            }
        }
        /* An expiry window of 0 disables expiry entirely, keeping the old
         * never-drop behaviour for anyone who wants it. */
        if (this->learned_server_expiry_ms > 0)
        {
            /* Clamped rather than computed as (now - last_seen) > window so the
             * subtraction cannot wrap on a clock whose epoch is inside the
             * window (a fake clock starting at 0, or a freshly booted host):
             * before the window has elapsed at all, nothing is expirable. */
            uint64_t cutoff = now > this->learned_server_expiry_ms ? now - this->learned_server_expiry_ms : 0;
            std::list<std::shared_ptr<fss_server>> doomed = take_stale_servers(this->servers, cutoff);
            doomed.splice(doomed.end(), take_stale_servers(this->reconnect_servers, cutoff));
            expired_count = doomed.size();
            for (auto const &server : doomed)
            {
                FSS_LOG_WARN("client", "Server " << server->getAddress() << ":" << server->getPort()
                                                 << " absent from server lists for " << this->learned_server_expiry_ms
                                                 << "ms, dropping it");
            }
            /* Teardown is deferred to attemptReconnect(): see expired_servers
             * in the header — this runs on a recv thread, possibly the one
             * belonging to the server being expired. */
            this->expired_servers.splice(this->expired_servers.end(), doomed);
        }
        learned_count = count_learned(this->servers) + count_learned(this->reconnect_servers);
    }
    /* Learn outside the lock: addServer() re-acquires it. */
    bool cap_reached = false;
    for (auto const &server_entry : to_learn)
    {
        if (learned_count >= this->max_learned_servers)
        {
            cap_reached = true;
            break;
        }
        this->addLearnedServer(server_entry.first, server_entry.second);
        learned_count++;
    }
    if (cap_reached)
    {
        bool report = false;
        {
            std::scoped_lock lock(this->servers_lock);
            report = !this->learned_cap_logged;
            this->learned_cap_logged = true;
        }
        if (report)
        {
            FSS_LOG_WARN("client",
                         "Refusing to learn more than " << this->max_learned_servers << " servers from server lists");
        }
    }
    if (expired_count > 0)
    {
        this->updateConfigured();
    }
}

static auto status_for_server_count(size_t count) -> flight_safety_system::client_ssl::connection_status
{
    switch (count)
    {
        case 0: return flight_safety_system::client_ssl::CLIENT_CONNECTION_STATUS_DISCONNECTED;
        case 1: return flight_safety_system::client_ssl::CLIENT_CONNECTION_STATUS_CONNECTED_1_SERVER;
        default: return flight_safety_system::client_ssl::CLIENT_CONNECTION_STATUS_CONNECTED_2_OR_MORE;
    }
}

void flight_safety_system::client_ssl::fss_client::serverRequiresReconnect(
    flight_safety_system::client_ssl::fss_server *server)
{
    /* Perform the list mutation under the lock, then derive the connection
     * status from the resulting server count and invoke the virtual callback
     * outside the lock.  connectionStatusChange() may re-enter the client
     * (e.g. call sendMsgAll or attemptReconnect), so the lock must not be
     * held when it is called. */
    size_t count = 0;
    bool found = false;
    {
        std::scoped_lock lock(this->servers_lock);
        for (auto it = this->servers.begin(); it != this->servers.end(); ++it)
        {
            if (it->get() == server)
            {
                this->reconnect_servers.push_back(std::move(*it));
                this->servers.erase(it);
                found = true;
                break;
            }
        }
        count = this->servers.size();
    }
    if (!found && server != nullptr)
    {
        /* Not in the live list, so this is a connection that died in the window
         * between its worker finishing a reconnect and attemptReconnect()
         * harvesting the result (see the decision file above). Discard the
         * stale success: promoting
         * it would move an already-dead connection into `servers`, where
         * nothing would flag it — a fresh connection starts with liveness
         * disarmed, so isServerTimedOut() would never fire. Dropping the flag
         * simply leaves it in reconnect_servers to be dialled again. */
        server->takeReconnectSucceeded();
    }
    this->connectionStatusChange(status_for_server_count(count));
}

void flight_safety_system::client_ssl::fss_client::notifyConnectionStatus()
{
    /* Read the server count under the lock, then release before invoking the
     * virtual user callback: connectionStatusChange() may re-enter the client
     * (e.g. call sendMsgAll), so holding the lock across it would deadlock. */
    size_t count = 0;
    {
        std::scoped_lock lock(this->servers_lock);
        count = this->servers.size();
    }
    this->connectionStatusChange(status_for_server_count(count));
}

void flight_safety_system::client_ssl::fss_client::connectionStatusChange(
    flight_safety_system::client_ssl::connection_status status __attribute__((unused)))
{
}

void flight_safety_system::client_ssl::fss_client::handleCommandFrom(
    const std::shared_ptr<flight_safety_system::transport::fss_message_asset_command> &msg,
    flight_safety_system::client_ssl::fss_server *origin __attribute__((unused)))
{
    /* Default: ignore the originating connection and fall back to the
     * connection-agnostic handler, so a subclass that only overrode
     * handleCommand() still sees the command. */
    this->handleCommand(msg);
}

void flight_safety_system::client_ssl::fss_client::handleCommand(
    const std::shared_ptr<flight_safety_system::transport::fss_message_asset_command> &msg __attribute__((unused)))
{
}

void flight_safety_system::client_ssl::fss_client::handlePositionReport(
    const std::shared_ptr<flight_safety_system::transport::fss_message_position_report> &msg __attribute__((unused)))
{
}

void flight_safety_system::client_ssl::fss_client::handleSMMSettings(
    const std::shared_ptr<flight_safety_system::transport::fss_message_smm_settings> &msg __attribute__((unused)))
{
}

void flight_safety_system::client_ssl::fss_client::handleRTTRequest(
    const std::shared_ptr<flight_safety_system::transport::fss_message_rtt_request> &msg __attribute__((unused)))
{
}

flight_safety_system::client_ssl::fss_server::fss_server(flight_safety_system::client_ssl::fss_client *t_client,
                                                         std::string t_address, uint16_t t_port, std::string t_ca,
                                                         std::string t_private_key, std::string t_public_key)
    : flight_safety_system::transport::fss_message_cb(nullptr), client(t_client), address(std::move(t_address)),
      port(t_port), ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)),
      public_key_file(std::move(t_public_key))
{
}

flight_safety_system::client_ssl::fss_server::~fss_server()
{
    /* Single teardown entry point: disconnect() stops the outbound worker
     * (shutting the socket down first so a blocked send returns) and clears the
     * connection. It is idempotent, so this is safe whether or not disconnect()
     * was already called. */
    fss_server::disconnect();
}

void flight_safety_system::client_ssl::fss_server::startOutboundWorkerLocked()
{
    if (this->outbound_stopping || this->outbound_worker.joinable())
    {
        return;
    }
    this->outbound_worker = std::thread(&fss_server::outboundWorkerRun, this);
}

auto flight_safety_system::client_ssl::fss_server::waitForOutboundWork()
    -> flight_safety_system::client_ssl::fss_server::outbound_work
{
    std::unique_lock<std::mutex> lock(this->outbound_lock);
    this->outbound_cv.wait(lock, [this]() -> bool {
        return this->outbound_stopping || this->out_reconnect_pending || !this->pending_sends.empty();
    });
    outbound_work work;
    if (this->outbound_stopping)
    {
        work.stop = true;
        return work;
    }
    work.reconnect = this->out_reconnect_pending;
    this->out_reconnect_pending = false;
    /* Hand the whole queue over rather than moving element by element: this is
     * a pointer swap, it cannot throw, and it leaves nothing to copy under the
     * lock. The clear() is still required — a moved-from container is valid but
     * unspecified, and pending_sends must be empty for the wait predicate.
     *
     * Element-wise (vector::assign over move_iterators) also tripped a GCC 14
     * -Wnull-dereference false positive on the inlined shared_ptr swap chain,
     * which is -Werror on Debian trixie. Not the reason for the change, but it
     * is the reason not to quietly revert it. */
    work.sends = std::move(this->pending_sends);
    this->pending_sends.clear();
    return work;
}

void flight_safety_system::client_ssl::fss_server::outboundWorkerRun()
{
    for (;;)
    {
        auto work = this->waitForOutboundWork();
        /* Queued telemetry is best-effort: a disconnecting client has nothing
         * left to say, and anything unsent is superseded by the next report.
         * Exit promptly on stop so a join never waits on a send. */
        if (work.stop)
        {
            return;
        }
        if (work.reconnect)
        {
            /* Blocking: connect() plus the TLS handshake, and the retirement of
             * any previous connection. Doing it here is the whole point — the
             * caller's thread no longer waits on it, so an unreachable server
             * cannot delay reconnection to a healthy one. */
            bool ok = this->reconnect();
            {
                std::scoped_lock guard(this->outbound_lock);
                this->reconnect_busy = false;
            }
            /* Publish success only now: reconnect() has sent the version and
             * identity handshake by the time it returns true, so the client can
             * safely promote this server to its live list (see the header). */
            if (ok)
            {
                this->reconnect_succeeded.store(true, std::memory_order_release);
            }
        }
        for (const auto &packed : work.sends)
        {
            /* Re-read the connection each time rather than caching it: a
             * reconnect can swap it underneath us, and a cleared one must send
             * nothing rather than fault. sendPacked copies the shared frame and
             * stamps this connection's own id into the copy (todo/55). */
            auto active_conn = this->getConnection();
            if (active_conn == nullptr)
            {
                continue;
            }
            active_conn->sendPacked(packed);
        }
    }
}

void flight_safety_system::client_ssl::fss_server::queueSend(
    const std::shared_ptr<const flight_safety_system::transport::buf_len> &packed)
{
    if (packed == nullptr)
    {
        return;
    }
    bool report_drop = false;
    {
        std::scoped_lock guard(this->outbound_lock);
        if (this->outbound_stopping)
        {
            return;
        }
        /* Drop-oldest, not drop-newest: telemetry only gets less useful with
         * age, so a backlogged server should shed its stalest report and keep
         * the freshest one. */
        while (this->pending_sends.size() >= max_pending_sends)
        {
            this->pending_sends.pop_front();
            this->sends_dropped++;
            /* One line per backlog episode, not per dropped frame: a wedged
             * server drops continuously, and the running total is available
             * from getDroppedSends(). Re-armed by reconnect(), so a server that
             * wedges again after coming back says so again. */
            if (!this->sends_drop_logged)
            {
                this->sends_drop_logged = true;
                report_drop = true;
            }
        }
        this->pending_sends.push_back(packed);
        this->startOutboundWorkerLocked();
    }
    if (report_drop)
    {
        FSS_LOG_WARN("client", "Outbound queue full for " << this->address << ":" << this->port
                                                          << ", dropping oldest telemetry for this server");
    }
    this->outbound_cv.notify_one();
}

void flight_safety_system::client_ssl::fss_server::queueReconnect()
{
    /* Refresh the client snapshot here, on the caller's thread, so the worker
     * never has to touch the client (see the snapshot members in the header).
     * Doing it per attempt is also what keeps a configuration change picked up
     * as promptly as reading the client live used to. */
    this->refreshClientConfig();
    {
        std::scoped_lock guard(this->outbound_lock);
        if (this->outbound_stopping || this->reconnect_busy)
        {
            return;
        }
        this->reconnect_busy = true;
        this->out_reconnect_pending = true;
        this->startOutboundWorkerLocked();
    }
    this->outbound_cv.notify_one();
}

auto flight_safety_system::client_ssl::fss_server::takeReconnectSucceeded() -> bool
{
    return this->reconnect_succeeded.exchange(false, std::memory_order_acquire);
}

auto flight_safety_system::client_ssl::fss_server::getDroppedSends() -> uint64_t
{
    std::scoped_lock guard(this->outbound_lock);
    return this->sends_dropped;
}

void flight_safety_system::client_ssl::fss_server::requestOutboundStop()
{
    {
        std::scoped_lock guard(this->outbound_lock);
        if (this->outbound_stopping)
        {
            return;
        }
        this->outbound_stopping = true;
        /* Drop any reconnect that was queued but not yet started, and release
         * the in-flight latch: a worker still inside reconnect() will clear it
         * again on the way out, and leaving it set would make every later
         * queueReconnect() a no-op for a server that came back up. */
        this->out_reconnect_pending = false;
        this->reconnect_busy = false;
    }
    this->outbound_cv.notify_all();
}

void flight_safety_system::client_ssl::fss_server::stopOutboundWorker()
{
    this->requestOutboundStop();
    if (!this->outbound_worker.joinable())
    {
        return;
    }
    if (this->outbound_worker.get_id() == std::this_thread::get_id())
    {
        /* The worker must never join itself; detach so it can finish. Mirrors
         * the recv-thread guard in fss_connection::disconnect(). */
        this->outbound_worker.detach();
        this->outbound_worker = std::thread();
        return;
    }
    try
    {
        this->outbound_worker.join();
    }
    catch (const std::system_error &e)
    {
        FSS_LOG_ERROR("client", "outbound_worker.join() failed, detaching: " << e.what());
        this->outbound_worker.detach();
        this->outbound_worker = std::thread();
    }
}

void flight_safety_system::client_ssl::fss_server::disconnect()
{
    /* Order matters (todo/21, todo/52): signal the worker to stop, then shut
     * the socket down -- not close it -- so a worker blocked in send() returns
     * while the descriptor number stays reserved, then join the worker, and
     * only then run the connection's disconnect(), which joins the recv thread
     * and performs the deferred close. Closing any earlier would free the fd
     * number for reuse while the worker may still be about to pass its stale
     * value to send() -- I/O into an unrelated session (todo/52). */
    this->requestOutboundStop();
    auto active_conn = this->getConnection();
    if (active_conn != nullptr)
    {
        active_conn->shutdownSocket(); // unblocks a stalled worker send and the recv thread
    }
    this->stopOutboundWorker();
    if (active_conn != nullptr)
    {
        active_conn->disconnect(); // joins the recv thread, then closes the fd
    }
    flight_safety_system::transport::fss_message_cb::disconnect(); // a second disconnect() here is a safe no-op
}

auto flight_safety_system::client_ssl::fss_server::getAddress() -> std::string
{
    return this->address;
}

auto flight_safety_system::client_ssl::fss_server::getPort() -> uint16_t
{
    return this->port;
}

auto flight_safety_system::client_ssl::fss_server::getClient() -> fss_client *
{
    return this->client;
}


void flight_safety_system::client_ssl::fss_server::sendIdentify()
{
    /* From the snapshot, not from the client: reconnect() calls this on the
     * outbound worker (see the snapshot members in the header). */
    bool non_aircraft = false;
    std::string asset_name;
    {
        std::scoped_lock guard(this->outbound_lock);
        non_aircraft = this->client_non_aircraft;
        asset_name = this->client_asset_name;
    }
    if (non_aircraft)
    {
        /* The server derives the identity from the peer cert's CN on this
         * path (todo/28), not from a message field. */
        this->getConnection()->sendMsg(
            std::make_shared<flight_safety_system::transport::fss_message_identity_non_aircraft>());
        return;
    }
    auto ident_msg = std::make_shared<flight_safety_system::transport::fss_message_identity>(asset_name);
    this->getConnection()->sendMsg(ident_msg);
}

void flight_safety_system::client_ssl::fss_server::refreshClientConfig()
{
    if (this->client == nullptr)
    {
        return;
    }
    /* Read the client OUTSIDE outbound_lock: these are virtual calls into
     * consumer code, and holding a lock the worker also takes across arbitrary
     * user code would be a hazard of its own. */
    auto timeout_ms = this->client->getTcpUserTimeoutMs();
    bool non_aircraft = this->client->isNonAircraft();
    auto asset_name = this->client->getAssetName();
    std::scoped_lock guard(this->outbound_lock);
    this->client_tcp_user_timeout_ms = timeout_ms;
    this->client_non_aircraft = non_aircraft;
    this->client_asset_name = std::move(asset_name);
}

void flight_safety_system::client_ssl::fss_server::sendVersion()
{
    auto version_msg = std::make_shared<flight_safety_system::transport::fss_message_version>();
    this->getConnection()->sendMsg(version_msg);
}

auto flight_safety_system::client_ssl::fss_server::reconnect_to() -> bool
{
    /* The client's requested send bound, taken from the snapshot rather than
     * read live: this runs on the outbound worker, which must not reach into
     * the client (see the snapshot members in the header). The snapshot is
     * refreshed at every queueReconnect(), so the bound still tracks a
     * configuration change per attempt, and still covers config-file servers,
     * programmatic connectTo, and servers learned from a server-list update
     * uniformly (todo/26). */
    unsigned int timeout_ms = 0;
    {
        std::scoped_lock guard(this->outbound_lock);
        timeout_ms = this->client_tcp_user_timeout_ms;
    }
    auto new_conn = flight_safety_system::transport_ssl::fss_connection_client::create(
        this->ca_file, this->private_key_file, this->public_key_file, this->getAddress(), this->getPort(), timeout_ms);
    if (new_conn == nullptr)
    {
        return false;
    }
    this->setConnection(new_conn);
    return true;
}

void flight_safety_system::client_ssl::fss_server::setClock(std::shared_ptr<flight_safety_system::IClock> t_clock)
{
    if (t_clock == nullptr)
    {
        return;
    }
    this->clock = std::move(t_clock);
}

/* Restore the backoff state to its cold-start values. Only ever called from
 * the thread driving reconnection (see the ownership note in the header). */
void flight_safety_system::client_ssl::fss_server::resetBackoff()
{
    this->last_tried = 0;
    this->retry_count = 0;
    this->retry_delay = retry_delay_start;
    this->effective_delay = retry_delay_start;
}

auto flight_safety_system::client_ssl::fss_server::reconnect() -> bool
{
    /* Consume a reset requested by the recv thread (connection closed):
     * retry immediately and restart the backoff progression. All backoff
     * fields are only ever touched on this thread (see the header), so the
     * atomic flag is the only cross-thread hand-off. */
    if (this->backoff_reset_requested.exchange(false))
    {
        this->resetBackoff();
    }

    uint64_t ts = this->clock->now_ms();
    uint64_t elapsed_time = ts - this->last_tried;

    /* Retire any previous connection properly (todo/65). Detach the handler
     * FIRST so the old connection's closed event queues on the dying
     * connection instead of reaching processMessage(), where it would be
     * misattributed to this server object and tear down the replacement
     * connection on the next reconnect tick. Then disconnect(), which
     * closes the fd and joins the recv thread: dropping the reference
     * alone leaks the connection (the recv-thread lambda holds it), and
     * the still-open session keeps this asset's identity claimed at the
     * server, which then rejects the re-identify as a duplicate for as
     * long as the orphan survives. Safe to join here: reconnect() runs on
     * the thread driving reconnection, which never holds servers_lock. */
    auto old_conn = this->getConnection();
    if (old_conn != nullptr)
    {
        old_conn->setHandler(nullptr);
        old_conn->disconnect();
        this->clearConnection();
    }

    if (elapsed_time > this->effective_delay)
    {
        this->retry_count++;
        if (this->retry_delay < retry_delay_cap)
        {
            /* Exponential backoff, clamped to the cap. Plain doubling would
             * overshoot (e.g. 16000 -> 32000 with a 30000 cap) before the
             * guard stops it on the next iteration. */
            this->retry_delay = std::min(this->retry_delay * 2, retry_delay_cap);
        }
        int64_t jitter_range = static_cast<int64_t>(this->retry_delay) / 4;
        std::uniform_int_distribution<int64_t> dist(-jitter_range, jitter_range);
        this->effective_delay = static_cast<uint64_t>(static_cast<int64_t>(this->retry_delay) + dist(this->rng));
        this->last_tried = ts;
        if (!this->reconnect_to())
        {
            this->clearConnection();
        }
        else
        {
            /* A fresh connection restarts liveness from the cold-connect
             * state (todo/65): disarmed until the first message arrives on
             * THIS connection (processMessage re-arms it). Merely
             * refreshing the timestamp would keep measuring the previous
             * connection's silence, so a quiet-but-healthy server would be
             * flagged as timed out again on the very next tick, forever.
             * Store before setHandler() so a message delivered immediately
             * cannot have its arming overwritten by this reset. */
            this->liveness_active.store(false, std::memory_order_relaxed);
            /* Re-arm the outbound worker. Unlike the server-side fss_client,
             * an fss_server outlives its connections: disconnect() stops the
             * worker, but this object can come back up on a new connection and
             * must be able to send again. Safe here because reconnect() and
             * disconnect() both belong to the reconnect-driving thread and are
             * never concurrent (see the ownership note in the header); the
             * previous worker has already been joined by stopOutboundWorker().
             * The worker itself is started lazily by the first queueSend(). */
            {
                std::scoped_lock guard(this->outbound_lock);
                this->outbound_stopping = false;
                this->sends_drop_logged = false;
            }
            this->getConnection()->setHandler(this);
            /* Protocol version handshake must be the first message
             * exchanged after TLS connect, before identity. */
            this->sendVersion();
            this->sendIdentify();
            this->resetBackoff();
            return true;
        }
    }
    return false;
}

auto flight_safety_system::client_ssl::fss_server::isServerTimedOut() -> bool
{
    /* Acquire pairs with the release in processMessage(): once the armed
     * flag is observed, the timestamp stored before it is visible too, so
     * arming can never be seen with a stale/zero timestamp (which would
     * read as an instant timeout). */
    if (!this->liveness_active.load(std::memory_order_acquire))
    {
        return false;
    }
    return (this->clock->now_ms() - this->last_message_received_time.load(std::memory_order_relaxed)) >
           this->server_timeout_ms;
}

void flight_safety_system::client_ssl::fss_server::processMessage(
    std::shared_ptr<flight_safety_system::transport::fss_message> msg)
{
    if (msg == nullptr)
    {
        return;
    }
#ifdef DEBUG
    std::cout << "Got message " << msg->getType() << std::endl;
#endif
    if (msg->getType() == flight_safety_system::transport::message_type_closed)
    {
        /* Connection has been closed, schedule reconnection. This runs on
         * the recv thread, which must not touch the backoff fields directly
         * (they belong to the reconnect-driving thread — see the header);
         * request the reset via the atomic flag instead. */
        this->backoff_reset_requested.store(true);
        this->getClient()->serverRequiresReconnect(this);
        return;
    }
    else
    {
        /* Timestamp first, then arm with release (paired with the acquire
         * in isServerTimedOut): a reader observing the armed flag is
         * guaranteed a timestamp at least this fresh. Arming before the
         * timestamp allowed a window where the flag was set but the
         * timestamp still held its previous (or zero) value — an instant
         * spurious timeout. */
        this->last_message_received_time.store(this->clock->now_ms(), std::memory_order_relaxed);
        this->liveness_active.store(true, std::memory_order_release);
        switch (msg->getType())
        {
            case flight_safety_system::transport::message_type_unknown:
            case flight_safety_system::transport::message_type_closed:
            case flight_safety_system::transport::message_type_identity:
            case flight_safety_system::transport::message_type_identity_non_aircraft:
            case flight_safety_system::transport::message_type_identity_required: break;
            case flight_safety_system::transport::message_type_version: {
                auto version_msg = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_version>(msg);
                if (version_msg != nullptr)
                {
                    uint16_t peer_version = version_msg->getProtocolVersion();
                    uint16_t peer_min = version_msg->getMinSupportedVersion();
                    if (peer_version < flight_safety_system::transport::FSS_PROTOCOL_MIN_VERSION ||
                        peer_min > flight_safety_system::transport::FSS_PROTOCOL_VERSION)
                    {
                        FSS_LOG_ERROR("client",
                                      "Server protocol version incompatible: peer="
                                          << peer_version << " peer_min=" << peer_min
                                          << " us=" << flight_safety_system::transport::FSS_PROTOCOL_VERSION
                                          << " us_min=" << flight_safety_system::transport::FSS_PROTOCOL_MIN_VERSION);
                        this->getClient()->serverRequiresReconnect(this);
                        return;
                    }
                    uint16_t negotiated = std::min(peer_version, flight_safety_system::transport::FSS_PROTOCOL_VERSION);
                    this->getConnection()->setNegotiatedVersion(negotiated);
                    this->getConnection()->setNegotiatedFeatureFlags(
                        flight_safety_system::transport::negotiateFeatureFlags(version_msg->getFeatureFlags()));
                }
            }
            break;
            case flight_safety_system::transport::message_type_rtt_request: {
                /* Send a response. When the RTT clock-offset capability was
                 * negotiated, stamp our wall clock so the server can estimate
                 * our clock offset (todo/17 item 3); otherwise reply in the
                 * legacy form (no timestamp). getConnection() may be null (e.g.
                 * mid-teardown), so guard it the same way sendMsg does. */
                auto active_conn = this->getConnection();
                bool report_clock =
                    active_conn != nullptr && (active_conn->getNegotiatedFeatureFlags() &
                                               flight_safety_system::transport::FSS_FEATURE_RTT_OFFSET) != 0;
                auto skewed_timestamp = this->getClient()->getSkewedTimestamp();
                auto reply_msg =
                    report_clock
                        ? std::make_shared<flight_safety_system::transport::fss_message_rtt_response>(msg->getId(),
                                                                                                      skewed_timestamp)
                        : std::make_shared<flight_safety_system::transport::fss_message_rtt_response>(msg->getId());
                this->sendMsg(reply_msg);
                /* Notify after replying: the reply is the time-critical
                 * half, the hook only observes (see fss-client-ssl.hpp). */
                auto rtt_req_msg =
                    std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_rtt_request>(msg);
                if (rtt_req_msg != nullptr)
                {
                    this->getClient()->handleRTTRequest(rtt_req_msg);
                }
            }
            break;
            case flight_safety_system::transport::message_type_rtt_response:
                /* Currently we don't send any rtt requests */
                break;
            case flight_safety_system::transport::message_type_position_report: {
                /* Servers will be relaying position reports, so this is another asset */
                auto position_msg =
                    std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_position_report>(msg);
                if (position_msg != nullptr)
                {
                    this->getClient()->handlePositionReport(position_msg);
                }
            }
            break;
            case flight_safety_system::transport::message_type_system_status:
            /* Servers don't currently send status reports */
            case flight_safety_system::transport::message_type_search_status:
                /* Servers don't have a search status to report */
                break;
            case flight_safety_system::transport::message_type_command: {
                auto command_msg =
                    std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_asset_command>(msg);
                if (command_msg != nullptr)
                {
                    /* Pass the originating server so an override can reply to
                     * this specific connection (command-ack id and negotiated
                     * feature flags are per-connection). */
                    this->getClient()->handleCommandFrom(command_msg, this);
                }
            }
            break;
            case flight_safety_system::transport::message_type_server_list: {
                auto server_list_msg =
                    std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_server_list>(msg);
                if (server_list_msg != nullptr)
                {
                    this->getClient()->updateServers(server_list_msg);
                }
            }
            break;
            case flight_safety_system::transport::message_type_smm_settings: {
                auto smm_settings_msg =
                    std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_smm_settings>(msg);
                if (smm_settings_msg != nullptr)
                {
                    this->getClient()->handleSMMSettings(smm_settings_msg);
                }
            }
            break;
            case flight_safety_system::transport::message_type_command_ack:
                /* Command acks flow FMU -> server (todo/17 item 1). The bundled
                 * client is the command sender, not a recipient, so it never
                 * needs to consume one. */
                break;
        }
    }
}
