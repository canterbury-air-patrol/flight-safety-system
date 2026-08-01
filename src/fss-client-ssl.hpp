#include <fss-transport-ssl.hpp>
#include <fss.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

namespace flight_safety_system {
namespace client_ssl {

class fss_client;
class fss_server;

/* How long a server learned from a server-list broadcast may go unmentioned
 * before the client drops it (docs/decisions/66-67-client-outbound-fanout.md).
 * Servers broadcast their list every 15 s, so the default is four rounds: long
 * enough that a missed or delayed broadcast expires nothing, short enough that
 * a decommissioned server stops costing reconnect attempts within a minute.
 * Config field "learned_server_expiry_ms". */
constexpr uint64_t default_learned_server_expiry_ms = 60000;
/* Ceiling on how many servers a client will learn from broadcasts. The list is
 * authenticated (it arrives over mTLS from a server that chained to our CA), so
 * this is not an unauthenticated attack surface — it bounds what a trusted but
 * misconfigured server can grow the connection set to. Config field
 * "max_learned_servers". Servers from the config file do not count against it
 * and are never expired: they are the operator's declared intent and must
 * survive an outage that empties every broadcast list. */
constexpr size_t default_max_learned_servers = 16;

enum connection_status {
    CLIENT_CONNECTION_STATUS_UNKNOWN,
    CLIENT_CONNECTION_STATUS_CONNECTED_1_SERVER,
    CLIENT_CONNECTION_STATUS_CONNECTED_2_OR_MORE,
    CLIENT_CONNECTION_STATUS_DISCONNECTED,
};

class fss_client {
private:
    std::string asset_name{""};
    /* When true, sendIdentify() sends message_type_identity_non_aircraft
     * instead of the aircraft identity (todo/28). The server derives the
     * asset name from the peer cert's CN in that path, not from a message
     * field, so there is no non-aircraft counterpart to asset_name. */
    bool non_aircraft{false};
    /* Added to the wall-clock timestamp reported in an RTT response, when
     * the server negotiated the clock-offset capability (todo/33). Lets an
     * e2e test drive a client with a deliberately skewed clock without
     * needing root/faketime. Positive = client clock ahead of real time. */
    int64_t clock_offset_ms{0};
    /* TCP_USER_TIMEOUT requested for every server connection this client
     * makes (docs/decisions/26-client-send-timeout.md): the bound on how
     * long a blocking send() can stall
     * into a half-dead server before the kernel errors the connection out.
     * A flight-safety client (e.g. cap-fmu) can set this well below the
     * 30 s default so a wedged send worker recovers on its own clock.
     * Config field "tcp_user_timeout_ms"; consulted at (re)connect time. */
    unsigned int tcp_user_timeout_ms{flight_safety_system::transport::default_tcp_user_timeout_ms};
    std::string ca_file{""};
    std::string private_key_file{""};
    std::string public_key_file{""};
    std::list<std::shared_ptr<flight_safety_system::client_ssl::fss_server>> servers{};
    std::list<std::shared_ptr<flight_safety_system::client_ssl::fss_server>> reconnect_servers{};
    /* Servers removed from both lists by expiry and awaiting
     * teardown. updateServers() runs on a recv thread and the server it expires
     * can be the very one the list arrived on, so disconnecting there would
     * have that thread join itself; attemptReconnect() — which already owns
     * connection lifecycle — drains this instead. Guarded by servers_lock. */
    std::list<std::shared_ptr<flight_safety_system::client_ssl::fss_server>> expired_servers{};
    /* servers_lock guards the server lists and the derived `configured` flag.
     * It does NOT guard asset_name, which is configuration state set before the
     * client is used concurrently (mutable so the const isConfigured() can lock
     * it to read the flag). */
    mutable std::mutex servers_lock{};
    bool configured{false}; // guarded by servers_lock
    /* Expiry window and cap for servers learned from broadcasts; see
     * the constants above. Configuration state, set before concurrent use. */
    uint64_t learned_server_expiry_ms{default_learned_server_expiry_ms};
    size_t max_learned_servers{default_max_learned_servers};
    /* One WARN per process when the cap first refuses an advertised server —
     * the silence was half the problem this fixed. Guarded by servers_lock. */
    bool learned_cap_logged{false};
    /* Drives learned-server expiry. Injectable so the timing is testable, and a
     * clock rather than a tick count deliberately (todo/70): this is the only
     * new timekeeping in the client and it should not repeat that pattern. */
    std::shared_ptr<flight_safety_system::IClock> clock{std::make_shared<flight_safety_system::MonotonicClock>()};
    /* Recompute `configured` from the current asset name + server lists. Called
     * from every path that sets the name or adds a server so isConfigured()
     * stays accurate however the client was built, not just the file ctor.
     * Takes servers_lock itself, so callers must not already hold it. */
    void updateConfigured();
    void notifyConnectionStatus();
    /* How many live servers have actually admitted this client (todo/79).
     * Precondition: servers_lock held. This — not servers.size() — is what
     * connectionStatusChange() reports; see serverAdmitted().
     *
     * Derived on demand, and a cached admitted_count member was considered and
     * REJECTED (PR 418 review). serverRequiresReconnect() is not the only way a
     * server leaves the live list: updateServers() splices expired entries
     * straight out of `servers` into `expired_servers` under this same lock,
     * bypassing it. A counter would need updating there too, and missing it
     * would leave the count permanently high — the client reporting CONNECTED
     * for a server that no longer exists, which is the very failure todo/79
     * removes, let back in by another door. The scan it avoids is over a list
     * bounded by max_learned_servers (16) plus the configured entries, and runs
     * only on status-change edges: serverAdmitted() returns before counting
     * once a server is already admitted, so this is not a per-message cost. */
    auto countAdmittedLocked() const -> size_t;
    virtual void connectionStatusChange(flight_safety_system::client_ssl::connection_status status);
protected:
    void setAssetName(std::string t_asset_name);
    void setNonAircraft(bool t_non_aircraft);
    void setClockOffsetMs(int64_t t_offset_ms);
    /* Call before connecting (configuration state, like the setters above);
     * an already-established connection keeps the bound it connected with. */
    void setTcpUserTimeoutMs(unsigned int t_timeout_ms);
    /* Configuration state, like the setters above. 0 disables expiry, keeping
     * the previous behaviour of never dropping a learned server. */
    void setLearnedServerExpiryMs(uint64_t t_expiry_ms);
    void setMaxLearnedServers(size_t t_max);
    void addServer(const std::shared_ptr<fss_server> &server);
    /* Add a server learned from a server-list broadcast, as distinct from one
     * the operator configured: only these are subject to the expiry window and
     * the cap. Separate entry point rather than an extra parameter on
     * connectTo() so existing overriders of that virtual are unaffected. */
    void addLearnedServer(const std::string &t_address, uint16_t t_port);
public:
    explicit fss_client(const std::string &config_file);
    explicit fss_client();
    explicit fss_client(std::string t_ca, std::string t_private_key, std::string t_public_key);
    fss_client(fss_client &) = delete;
    fss_client(fss_client &&) = delete;
    auto operator=(fss_client &) -> fss_client & = delete;
    auto operator=(fss_client &&) -> fss_client & = delete;
    virtual ~fss_client();
    /* Register a server the operator configured, optionally dialling it now.
     * The dial on this path IS synchronous — it is configuration-time, on the
     * caller's own thread — unlike the ones attemptReconnect() schedules. A
     * server added here is never expired
     * (docs/decisions/66-67-client-outbound-fanout.md). */
    virtual void connectTo(const std::string &t_address, uint16_t t_port, bool connect);
    /* One pass of connection maintenance; call it periodically (the shipped
     * example does so at 1 Hz). NON-BLOCKING
     * (docs/decisions/66-67-client-outbound-fanout.md): it schedules a dial on
     * each pending server's own worker and promotes the ones whose dial has
     * since succeeded, so a pass no longer costs the sum of every unreachable
     * server's connect and handshake timeouts. Consequently a server does not
     * become live within the same call that first schedules it — the following
     * pass is the one that promotes it. Also the point at which servers dropped
     * by server-list expiry are torn down, so it must keep being called for
     * that to happen. */
    virtual void attemptReconnect();
    virtual void disconnect();
    /* Fan a message out to every currently connected server. NON-BLOCKING
     * (docs/decisions/66-67-client-outbound-fanout.md): the frame is packed
     * once here and handed to each server's own outbound worker, which
     * performs the blocking write. A server that completes TLS and then stops
     * reading can therefore only stall its own telemetry, never the fan-out to
     * the healthy ones. Loss-tolerant by design: each worker's queue is
     * bounded and drops the OLDEST frame under sustained backlog (see
     * fss_server::getDroppedSends()), so a wedged server sheds stale telemetry
     * rather than growing without bound.
     * Packing once is also what keeps the todo/12 C8 invariant intact now that
     * the sends are concurrent: the packed frame is shared read-only and each
     * connection stamps its own sequence id into its own copy
     * (fss_connection::sendPacked), so no fss_message instance is shared. */
    virtual void sendMsgAll(const std::shared_ptr<flight_safety_system::transport::fss_message> &msg);
    virtual auto getAssetName() -> std::string;
    virtual auto isNonAircraft() const -> bool { return this->non_aircraft; }
    virtual auto getClockOffsetMs() const -> int64_t { return this->clock_offset_ms; }
    virtual auto getTcpUserTimeoutMs() const -> unsigned int { return this->tcp_user_timeout_ms; }
    virtual auto getLearnedServerExpiryMs() const -> uint64_t { return this->learned_server_expiry_ms; }
    virtual auto getMaxLearnedServers() const -> size_t { return this->max_learned_servers; }
    /* How many servers this client currently knows of — live plus pending
     * reconnect — and how many of those it learned from a broadcast rather than
     * being configured with. Exposed because the failure this fixed was
     * invisible:
     * connectionStatusChange() reports only the count of LIVE servers, so a
     * client quietly carrying eleven decommissioned ones, and paying a connect
     * attempt for each every backoff interval, looked identical to a healthy
     * one. */
    auto getServerCount() const -> size_t;
    auto getLearnedServerCount() const -> size_t;
    /* Replace the clock driving learned-server expiry. Call before concurrent
     * use; a null clock is ignored (matching fss_server::setClock). */
    void setClock(std::shared_ptr<flight_safety_system::IClock> t_clock);
    /* fss_current_timestamp() + clock_offset_ms (todo/33): the single
     * source of truth for "what time does this client think it is", used
     * both for the RTT response's reported client clock and for any
     * message timestamp a skewed-clock test wants to be consistent with
     * it (e.g. a position report) -- a client whose clock is genuinely
     * wrong stamps everything with that wrong clock, not just RTT
     * responses. */
    virtual auto getSkewedTimestamp() const -> uint64_t;
    virtual auto isConfigured() const -> bool;
    virtual void serverRequiresReconnect(fss_server *server);
    /* Record that `server` has admitted this client, and report the resulting
     * connection status if that changed the admitted count (todo/79).
     * Idempotent: only the first call for a given connection notifies.
     *
     * A server joins the live list when its TCP/TLS connect succeeds, but
     * admission is a *later*, server-side decision — the server refuses a
     * client while its DB fail-safe is degraded, and refuses a duplicate
     * identity, both after the handshake the connect already counted as
     * success. Reporting a connection the server severs milliseconds later as
     * service made an aircraft leave its comms-loss failsafe and resume the
     * previous command, then re-enter it when the drop landed. Counting
     * admitted servers rather than connected ones is what stops that.
     *
     * Called from fss_server::processMessage() on the first message of a type
     * the server sends only to a client it has admitted. */
    void serverAdmitted(fss_server *server);
    virtual void updateServers(const std::shared_ptr<flight_safety_system::transport::fss_message_server_list> &msg);
    /* Called for every command, with the originating server (the connection the
     * command arrived on). A subclass that needs to reply to that specific
     * connection — e.g. to send a command-ack, whose id and negotiated feature
     * flags are per-connection — overrides this and uses
     * origin->sendMsg()/origin->getConnection(). The default delegates to the
     * connection-agnostic handleCommand() below, so a subclass that only cares
     * about the command (the common case) overrides that one and is unaffected.
     * Distinct name rather than an overload so neither hides the other. */
    virtual void
    handleCommandFrom(const std::shared_ptr<flight_safety_system::transport::fss_message_asset_command> &msg,
                      flight_safety_system::client_ssl::fss_server *origin);
    virtual void handleCommand(const std::shared_ptr<flight_safety_system::transport::fss_message_asset_command> &msg
                               __attribute__((unused)));
    virtual void
    handlePositionReport(const std::shared_ptr<flight_safety_system::transport::fss_message_position_report> &msg
                         __attribute__((unused)));
    virtual void handleSMMSettings(const std::shared_ptr<flight_safety_system::transport::fss_message_smm_settings> &msg
                                   __attribute__((unused)));
    /* Notification that an RTT liveness request arrived; the reply has
     * already been sent by the transport handler, so an override observes
     * rather than answers. Exists so a test client can surface the server's
     * liveness tick externally — e2e's DB black-hole test asserts these keep
     * arriving while both DB connections are wedged
     * (docs/decisions/46-no-statement-timeout.md). */
    virtual void handleRTTRequest(const std::shared_ptr<flight_safety_system::transport::fss_message_rtt_request> &msg
                                  __attribute__((unused)));
};

class fss_server : public flight_safety_system::transport::fss_message_cb {
private:
    fss_client *client;
    std::string address;
    uint16_t port;
    std::string ca_file{""};
    std::string private_key_file{""};
    std::string public_key_file{""};
    /* Backoff bookkeeping (last_tried, retry_count, retry_delay,
     * effective_delay, rng) is touched only from the single thread that runs
     * reconnect(): this server's outbound worker when the reconnection was
     * scheduled by attemptReconnect() (the normal path), or the caller's own
     * thread on the configuration-time connectTo(..., true). Those two never
     * overlap — connectTo builds a server that is not yet in any list, so
     * nothing can have queued a reconnect for it. The recv thread must NOT
     * write these directly: when a connection closes it requests a reset via
     * the atomic backoff_reset_requested flag, which reconnect() consumes.
     * effective_delay is the exception: getEffectiveDelay() is a public read
     * from other threads, so it is atomic. */
    uint64_t last_tried{0};
    uint64_t retry_count{0};
    static constexpr uint64_t retry_delay_start = 1000;
    static constexpr uint64_t retry_delay_cap = 30000;
    uint64_t retry_delay{retry_delay_start};
    std::atomic<uint64_t> effective_delay{retry_delay_start};
    std::mt19937 rng{std::random_device{}()};
    std::atomic<bool> backoff_reset_requested{false};
    void resetBackoff();
    std::shared_ptr<flight_safety_system::IClock> clock{std::make_shared<flight_safety_system::MonotonicClock>()};
    std::atomic<bool> liveness_active{false};
    std::atomic<uint64_t> last_message_received_time{0};
    uint64_t server_timeout_ms{30000};
    /* Learned-server bookkeeping
     * (docs/decisions/66-67-client-outbound-fanout.md). Owned by the fss_client
     * rather than by this object: both are read and written only under
     * fss_client's servers_lock, or before the server has been published.
     * `learned` is true only for a server discovered from a server-list
     * broadcast — a config-file or programmatic entry stays false and is
     * therefore never expired. */
    bool learned{false};
    uint64_t last_seen_ms{0};
    /* True once this connection has been admitted by the server, as distinct
     * from merely connected to it (todo/79). Owned by the fss_client under
     * servers_lock, like `learned` above. Cleared whenever the connection ends,
     * so each new connection has to earn it again — a reconnect into a server
     * that refuses the client must not inherit the previous session's
     * admission. */
    bool admitted{false};
    /* Per-server outbound writer
     * (docs/decisions/66-67-client-outbound-fanout.md). The caller's thread
     * schedules *what* to send; this worker performs the blocking socket
     * write, so one black-holed server can only stall its own telemetry, never
     * the fan-out to every other server. The aircraft-side mirror of the
     * server's per-client worker (todo/21 + todo/36). Every member below is
     * guarded by outbound_lock. */
    std::mutex outbound_lock{};
    std::condition_variable outbound_cv{};
    bool outbound_stopping{false};
    /* Reconnection is dispatched onto the same worker: reconnect() blocks for a
     * connect() plus a TLS handshake, and doing that serially for every entry
     * on the caller's thread delayed reconnection to a healthy server by the
     * sum of every unreachable one's timeout.
     * out_reconnect_pending is the queued flag; reconnect_busy stays set from
     * the moment one is queued until the worker has finished it, so a 1 Hz
     * attemptReconnect() cannot pile attempts up behind a 10 s handshake. */
    bool out_reconnect_pending{false};
    bool reconnect_busy{false};
    /* Snapshot of the fss_client configuration the (re)connect path needs,
     * refreshed on the application's own thread (fss_client::connectTo(), and
     * every queueReconnect()). The outbound worker must NOT reach back through
     * `client`: an fss_client subclass is destroyed derived-part-first, so a
     * virtual call arriving from a worker during teardown races the vptr
     * rewrite. That is not theoretical — ThreadSanitizer reports it as a data
     * race in ~fss_client(), and it is undefined behaviour whichever vtable the
     * call happens to land in. Keeping the worker's reads inside its own object
     * removes the cross-object access rather than trying to time it, which is
     * the only version that stays correct for a subclass this library has never
     * seen. Guarded by outbound_lock. */
    unsigned int client_tcp_user_timeout_ms{flight_safety_system::transport::default_tcp_user_timeout_ms};
    bool client_non_aircraft{false};
    std::string client_asset_name{};
    /* Set by the worker only AFTER reconnect() has returned true — i.e. after
     * the version + identity handshake has been sent — and consumed by
     * fss_client::attemptReconnect(). Harvesting on connected() instead would
     * expose the window inside reconnect() between installing the connection
     * and sending the handshake, during which sendMsgAll() could put telemetry
     * ahead of the mandatory version message. */
    std::atomic<bool> reconnect_succeeded{false};
    /* Bounded, drop-oldest: telemetry is loss-tolerant, so under sustained
     * backlog into a half-dead server the OLDEST queued frame is dropped
     * rather than the queue growing without bound — the same policy and
     * reasoning as the server side's pending_position_relay. */
    static constexpr size_t max_pending_sends = 8;
    std::deque<std::shared_ptr<const flight_safety_system::transport::buf_len>> pending_sends{};
    /* Cumulative frames dropped by the cap above (never reset). Exposed so the
     * loss is observable rather than silent, mirroring
     * fss_connection::getDroppedMessages() for the inbound queue. */
    uint64_t sends_dropped{0};
    /* True once the WARN for the current backlog episode has been emitted, so a
     * continuously wedged server produces one line rather than one per frame.
     * Cleared by reconnect(). */
    bool sends_drop_logged{false};
    std::thread outbound_worker{};
    /* What the worker should do this wake-up. Returned by waitForOutboundWork()
     * so the worker performs the (blocking) sends with no lock held. */
    struct outbound_work {
        bool stop{false};
        bool reconnect{false};
        /* Same container type as pending_sends so waitForOutboundWork() can
         * hand the queue over with a whole-container move rather than copying
         * element by element. It is only ever iterated once, in order, so a
         * deque costs the consumer nothing over a vector. */
        std::deque<std::shared_ptr<const flight_safety_system::transport::buf_len>> sends{};
    };
    /* Block until there is work or a stop request, then atomically take and
     * clear the pending work. */
    auto waitForOutboundWork() -> outbound_work;
    /* The worker loop: waits for work, then performs the blocking send(s) off
     * the caller's thread. */
    void outboundWorkerRun();
    /* Idempotent: set the stop flag and wake the worker. The single place that
     * owns the stop signal, so disconnect() and stopOutboundWorker() cannot
     * drift apart. */
    void requestOutboundStop();
    /* Idempotent: request the stop (above) and join the worker. Safe to call
     * more than once and from any thread other than the worker itself. The
     * caller must have already shut the connection's socket down if the worker
     * might be blocked in a send, otherwise the join waits for the send
     * timeout. */
    void stopOutboundWorker();
    /* Start the worker if it is not already running and no stop has been
     * requested. Precondition: outbound_lock held. Started lazily rather than
     * in the constructor so an fss_server that never sends costs no thread,
     * and so the worker can never call a virtual (reconnect_to) before a
     * subclass constructor has finished. */
    void startOutboundWorkerLocked();
protected:
    virtual auto reconnect_to() -> bool;
public:
    fss_server(fss_client *t_client, std::string t_address, uint16_t t_port, std::string t_ca,
               std::string t_private_key, std::string t_public_key);
    fss_server(fss_server &other) = delete;
    fss_server(fss_server &&) = delete;
    auto operator=(fss_server &) -> fss_server & = delete;
    auto operator=(fss_server &&) -> fss_server & = delete;
    ~fss_server() override;
    void processMessage(std::shared_ptr<flight_safety_system::transport::fss_message> message) override;
    /* Queue a pre-packed frame for this server's outbound worker instead of
     * sending it inline (docs/decisions/66-67-client-outbound-fanout.md).
     * Returns immediately; the worker copies the shared frame and stamps this
     * connection's own sequence id into the copy (fss_connection::sendPacked),
     * which is what lets one frame be fanned out concurrently without
     * violating the todo/12 C8 invariant. Drops the oldest queued frame if the
     * queue is already full. Per-connection sends that must mutate the message
     * -- sendIdentify(), sendVersion(), the RTT reply -- deliberately keep
     * using the inline fss_message_cb::sendMsg() on the recv thread, where a
     * stall can only affect the one connection it belongs to. */
    void queueSend(const std::shared_ptr<const flight_safety_system::transport::buf_len> &packed);
    /* Schedule a reconnect on this server's outbound worker instead of dialling
     * inline (see the outbound-worker block above). Returns immediately, and
     * is a no-op while an attempt is
     * already queued or in flight, so calling it every tick is safe. The
     * backoff throttle still lives in reconnect() itself, so a queued attempt
     * inside the retry window costs a worker wake-up and nothing else. */
    void queueReconnect();
    /* Re-read the owning client's configuration into this server's snapshot
     * (see the snapshot members). Must be called on a thread where the client
     * is known alive and fully constructed; queueReconnect() and
     * fss_client::connectTo() both already do, so a consumer only needs this
     * after changing client configuration on an already-registered server. */
    void refreshClientConfig();
    /* Consume the "the queued reconnect succeeded" signal (clearing it).
     * fss_client::attemptReconnect() polls this to move the server from
     * reconnect_servers to servers, which keeps every list mutation on the
     * thread that owns servers_lock and out of the worker. */
    auto takeReconnectSucceeded() -> bool;
    /* Cumulative frames this server's bounded outbound queue has dropped. */
    auto getDroppedSends() -> uint64_t;
    /* Stops this server's outbound worker and then the connection. Overrides
     * fss_message_cb::disconnect so a stalled writer is unblocked and joined
     * before the connection is torn down (the todo/21 + todo/52 order). */
    void disconnect() override;
    virtual auto getAddress() -> std::string;
    virtual auto getPort() -> uint16_t;
    virtual auto reconnect() -> bool;
    virtual auto getClient() -> fss_client *;
    virtual void sendIdentify();
    virtual void sendVersion();
    void setClock(std::shared_ptr<flight_safety_system::IClock> t_clock);
    /* Learned-server accessors. See the members: the caller must hold
     * fss_client::servers_lock, or be working on a server not yet published
     * into either list. */
    auto isLearned() const -> bool { return this->learned; }
    void setLearned(bool t_learned) { this->learned = t_learned; }
    /* Admission state (todo/79). Same locking rule as the learned accessors
     * above: fss_client::serverAdmitted() and the paths that end a connection
     * are the only callers, and each holds servers_lock. */
    auto isAdmitted() const -> bool { return this->admitted; }
    void setAdmitted(bool t_admitted) { this->admitted = t_admitted; }
    auto getLastSeenMs() const -> uint64_t { return this->last_seen_ms; }
    void setLastSeenMs(uint64_t t_now_ms) { this->last_seen_ms = t_now_ms; }
    void setServerTimeoutMs(uint64_t ms) { this->server_timeout_ms = ms; }
    auto isServerTimedOut() -> bool;
    auto getEffectiveDelay() const -> uint64_t { return this->effective_delay; }
};


} // namespace client_ssl
} // namespace flight_safety_system