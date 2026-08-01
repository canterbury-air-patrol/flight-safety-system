#pragma once

#include "fss.hpp"
#include "fss-transport.hpp"
#include "db-write-queue.hpp"
#include "rate-limiter.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flight_safety_system {
namespace server {

constexpr int command_poll_ms = 100;

/* Default position-staleness window (ms). Shared so the fss_client member,
 * server_clients, and server.cpp config default cannot drift apart. */
constexpr uint64_t default_position_staleness_ms = 30000;

class smm_settings {
private:
    std::string address;
    secure_string username;
    secure_string password;
public:
    smm_settings(std::string t_address, secure_string t_username, secure_string t_password);
    auto getAddress() -> std::string;
    auto getUsername() -> const secure_string &;
    auto getPassword() -> const secure_string &;
};

class fss_server_details {
private:
    std::string address;
    uint16_t port;
public:
    fss_server_details(std::string t_address, uint16_t t_port);
    auto getAddress() -> std::string;
    auto getPort() -> uint16_t;
};

class asset_command {
private:
    uint64_t dbid;
    uint64_t timestamp;
    transport::fss_asset_command command{transport::fss_asset_command::asset_command_unknown};
    double latitude;
    double longitude;
    uint32_t altitude;
    /* False when the DB row's altitude was NULL: altitude is unsigned, so
     * unlike a NULL position (NaN) it has no in-band sentinel. sendCommand
     * refuses to dispatch an ALT command without a valid altitude. */
    bool altitude_valid;
public:
    asset_command(uint64_t t_dbid, uint64_t t_timestamp, const std::string &t_cmd, double t_latitude,
                  double t_longitude, uint32_t t_altitude, bool t_altitude_valid = true);
    auto getDBId() -> uint64_t;
    auto getTimeStamp() -> uint64_t;
    auto getCommand() -> transport::fss_asset_command;
    auto getLatitude() -> double;
    auto getLongitude() -> double;
    auto getAltitude() -> uint32_t;
    auto isAltitudeValid() -> bool;
};

/* Thrown by db_connection's write wrappers when the underlying SQL fails, so
 * db_write_queue's worker (the only production write path) can count the
 * failure toward the todo/34/45/47 fail-safe. Reads do not throw: a read that
 * could only produce a partial, misleading result reports it in its return
 * value instead (see getActiveServers), so no read caller needs a try block
 * to stay terminate-safe (todo/24). */
class database_error : public std::runtime_error {
public:
    explicit database_error(const std::string &what) : std::runtime_error(what) {}
};

/* Pure-virtual database seam: lets ClientSession be unit-tested against
 * an in-memory mock without a live PostgreSQL. db_connection implements
 * it in production; tests/mock_database.hpp implements it in Catch2. */
class IDatabase {
public:
    IDatabase() = default;
    IDatabase(const IDatabase &) = delete;
    IDatabase(IDatabase &&) = delete;
    auto operator=(const IDatabase &) -> IDatabase & = delete;
    auto operator=(IDatabase &&) -> IDatabase & = delete;
    virtual ~IDatabase() = default;
    /* nullopt means the lookup itself failed (DB read error); 0 means the
     * query ran and found no asset by that name; anything else is the id.
     * An explicit status, not an in-band sentinel, so a read outage isn't
     * indistinguishable from a fleet of unregistered assets (todo/60,
     * matches the getActiveServers() convention below). */
    virtual auto getAssetId(const std::string &name) -> std::optional<uint64_t> = 0;
    /* gps_fix_valid false marks the coordinates as the autopilot's
     * dead-reckoned estimate rather than a GPS-backed position, so fss-web can
     * label them instead of ageing them as if they were a fix. A NaN latitude
     * or longitude means the report carried no coordinates at all and stores
     * NULL geometry; a no-fix report is recorded either way, because "the
     * aircraft says it is blind" is what the operator needs and it is not
     * inferable from a position that merely stops advancing (todo/76). */
    virtual void recordPosition(uint64_t asset_id, double latitude, double longitude, uint32_t altitude,
                                bool gps_fix_valid) = 0;
    virtual void recordRtt(uint64_t asset_id, uint64_t rtt_ms) = 0;
    virtual void recordStatus(uint64_t asset_id, uint8_t bat_percent, uint32_t bat_mah_used, double bat_voltage) = 0;
    virtual void recordSearchStatus(uint64_t asset_id, uint64_t search_id, uint64_t completed, uint64_t total) = 0;
    /* Records the dispatch id (the per-connection message id the server stamped
     * on the command) against the command row, and reopens the row's ack cycle
     * (clears the ack columns): the stored ack always describes the latest
     * dispatch, and a terminal outcome is final only within its dispatch — see
     * recordCommandAck. Enqueued once per command per connection, not on every
     * resend (todo/68): the clear is what made the healthy path destroy and
     * rewrite a command's stored outcome every resend window. */
    virtual void recordCommandDispatch(uint64_t command_dbid, uint64_t dispatch_id) = 0;
    /* Stores a command ack against the command row named by its primary key.
     * The caller has already translated the wire's per-connection acked id to the
     * row it dispatched (todo/68), so this names exactly one row and carries no
     * scoping of its own. Terminal-transition policy (todo/48): a terminal outcome
     * (actioned/superseded/rejected/noop) is final for its dispatch — the write is
     * refused unless the stored state is empty or received, so neither a late
     * "received" nor a second terminal can rewrite a settled outcome; a redispatch
     * reopens the cycle. ack_state/ack_reason are the
     * fss_command_ack_outcome/fss_command_ack_reason ints. */
    virtual void recordCommandAck(uint64_t command_dbid, uint8_t ack_state, uint64_t ack_timestamp,
                                  uint8_t ack_reason) = 0;
    /* The asset's newest pending command, or nullopt when the read failed.
     * nullopt is NOT "no pending command": that is an engaged null pointer.
     * The distinction matters most on this read — it carries TERM and DISARM,
     * so a read outage reported as "nothing waiting" is the wrong failure
     * direction (todo/69). A caller that cannot act on the failure should
     * leave whatever pending command it already holds untouched. */
    virtual auto getCommand(uint64_t asset_id) -> std::optional<std::shared_ptr<asset_command>> = 0;
    /* Batched getCommand: the newest command for each id in asset_ids, keyed by
     * asset_id. Assets with no pending command are simply absent from the map,
     * so callers treat "absent" exactly as getCommand's engaged nullptr. Lets
     * the command poller issue one query per tick instead of one per client
     * (todo/23).
     *
     * nullopt means the read was cut short, and the partial map is discarded
     * rather than returned: in a partial map "absent" cannot be told from "not
     * read", which is precisely the confusion this convention exists to
     * prevent, so the caller must leave every pending command it holds alone
     * and wait for the next tick (todo/69). */
    virtual auto getCommands(const std::vector<uint64_t> &asset_ids)
        -> std::optional<std::unordered_map<uint64_t, std::shared_ptr<asset_command>>> = 0;
    /* The complete set of active servers, or nullopt when the read was cut
     * short (mid-cursor error, truncated row) and could only produce a
     * partial, misleading list. nullopt is NOT an empty list: the caller must
     * keep whatever list it already has rather than shipping the failure to
     * aircraft as "no servers". An explicit status, not an exception, so the
     * contract is visible at every call site (todo/24). */
    virtual auto getActiveServers() -> std::optional<std::vector<fss_server_details>> = 0;
    /* The asset's SMM settings, or nullopt when the read failed. nullopt is NOT
     * "no settings": a null pointer inside the optional is the genuinely-absent
     * answer, and only that one may overwrite a cached copy. Without the split,
     * one transient failure on the poller thread wipes an aircraft's settings
     * until a later read succeeds (todo/69, same convention as
     * getActiveServers above). */
    virtual auto getSmmSettings(uint64_t asset_id) -> std::optional<std::shared_ptr<smm_settings>> = 0;
    virtual auto isConnected() const -> bool = 0;
    virtual void tryReconnectIfNeeded() = 0;
};

class db_connection : public IDatabase {
private:
    /* Two independent ECPG connections so a stalled telemetry write cannot
     * block a command/config read. Each connection is guarded by its own
     * mutex and therefore only ever used by one thread at a time — the
     * thread-safe usage pattern ECPG documents. read_lock covers getAssetId,
     * getCommand, getCommands, getActiveServers and getSmmSettings; write_lock
     * covers the record* telemetry inserts. */
    std::mutex read_lock;
    std::mutex write_lock;
    /* Read without holding either mutex by isConnected(), and written from
     * reconnectOne() under the matching mutex, so they must be atomic to
     * avoid a data race. */
    std::atomic<bool> read_connected_{false};
    std::atomic<bool> write_connected_{false};
    std::string host_;
    int port_{5432};
    std::string user_;
    /* todo/43: held wiped (secure_string) rather than as a plain std::string
     * resident for the life of the connection. */
    secure_string pass_;
    std::string db_;
    auto connectOne(const char *conn_name) -> bool;
    void reconnectOne(const char *conn_name, std::atomic<bool> &connected_flag);
public:
    /* ECPG connection names, exposed so tests can target a specific
     * connection (e.g. force one closed) without duplicating the literals. */
    static constexpr const char *read_conn_name = "fss_read";
    static constexpr const char *write_conn_name = "fss_write";
    db_connection(std::string host, int port, std::string user, secure_string pass, std::string db);
    db_connection(db_connection &) = delete;
    db_connection(db_connection &&) = delete;
    auto operator=(db_connection &) -> db_connection & = delete;
    auto operator=(db_connection &&) -> db_connection & = delete;
    ~db_connection() override;
    auto getAssetId(const std::string &name) -> std::optional<uint64_t> override;
    void recordPosition(uint64_t asset_id, double latitude, double longitude, uint32_t altitude,
                        bool gps_fix_valid) override;
    void recordRtt(uint64_t asset_id, uint64_t rtt_ms) override;
    void recordStatus(uint64_t asset_id, uint8_t bat_percent, uint32_t bat_mah_used, double bat_voltage) override;
    void recordSearchStatus(uint64_t asset_id, uint64_t search_id, uint64_t completed, uint64_t total) override;
    void recordCommandDispatch(uint64_t command_dbid, uint64_t dispatch_id) override;
    void recordCommandAck(uint64_t command_dbid, uint8_t ack_state, uint64_t ack_timestamp,
                          uint8_t ack_reason) override;
    auto getCommand(uint64_t asset_id) -> std::optional<std::shared_ptr<asset_command>> override;
    auto getCommands(const std::vector<uint64_t> &asset_ids)
        -> std::optional<std::unordered_map<uint64_t, std::shared_ptr<asset_command>>> override;
    auto getActiveServers() -> std::optional<std::vector<fss_server_details>> override;
    auto getSmmSettings(uint64_t asset_id) -> std::optional<std::shared_ptr<smm_settings>> override;
    auto isConnected() const -> bool override;
    void tryReconnectIfNeeded() override;
    /* Startup gate: checks the connected database carries every column
     * server-db.pgc reads or writes, logging each problem it finds. Returns
     * false if the server must not start — a missing column, or a check that
     * could not be completed. A column merely wide enough to overflow a host
     * buffer is logged as a warning and does not stop the server. Deliberately
     * not on IDatabase: no client handler needs it, and a mock database has no
     * schema to check. See docs/decisions/73-startup-schema-verification.md. */
    auto verifySchema() -> bool;
};

class fss_client_rtt {
private:
    uint64_t timestamp;
    uint64_t reqid;
public:
    fss_client_rtt(uint64_t t_timestamp, uint64_t t_reqid);
    auto getTimeStamp() -> uint64_t;
    auto getRequestId() -> uint64_t;
};

class fss_client;

/* Server policy when a second connection identifies as an asset that
 * already has a live session (todo/31). mTLS proves the client cert, so a
 * duplicate is far more likely to be a stale reconnect than a stolen-key
 * hijack — but the default is still the conservative one, to avoid
 * flip-flopping sessions on a genuine misconfiguration (e.g. two real
 * assets sharing a cert). */
enum duplicate_identity_policy {
    /* Keep the existing session; refuse the new connection at identify. */
    duplicate_identity_reject_newcomer,
    /* Disconnect the existing session; the new connection proceeds. */
    duplicate_identity_evict_oldest,
};

class fss_client_handler {
public:
    virtual ~fss_client_handler() = default;
    virtual void clientDisconnected(fss_client *client) = 0;
    /* Sends msg to every client (bar `except`). fss_connection::sendMsg
     * stamps a per-connection id into the message instance it is given
     * (todo/12 C8), so implementations must never let two clients' sends run
     * concurrently against the same instance — either by iterating
     * sequentially on one thread (reusing one instance is fine there, since
     * each send fully completes before the next starts), or (todo/36) by
     * giving each recipient its own independent clone before fanning delivery
     * out across separate per-client threads. */
    virtual void broadcastMsg(const std::shared_ptr<transport::fss_message> &msg, fss_client *except = nullptr) = 0;
    /* Called from the identify path once asset_id is resolved, before
     * `newcomer` is marked identified. Returns false if `newcomer` must be
     * rejected outright (duplicate_identity_reject_newcomer hit an existing
     * live session for this asset_id); true otherwise — no conflict, or the
     * existing session was evicted (duplicate_identity_evict_oldest) and
     * `newcomer` may proceed. The caller publishes into cached_asset_id only
     * after a true return, so an implementation must make the duplicate
     * check atomic with an internal reservation of asset_id for `newcomer`
     * (todo/44) — a check against published ids alone races a concurrent
     * identify for the same asset — and release the reservation when the
     * client disconnects. Default no-op always returns true (no dedup), so
     * existing fss_client_handler test mocks need not implement this. */
    virtual auto resolveDuplicateIdentity(fss_client * /*newcomer*/, uint64_t /*asset_id*/) -> bool { return true; }
};

class fss_client : public transport::fss_message_cb {
private:
    std::atomic<bool> identified{false};
    std::atomic<bool> aircraft{false};
    std::atomic<bool> version_received{false};
    std::atomic<uint64_t> expected_seq{0};
    std::atomic<uint64_t> cached_asset_id{0};
    std::shared_ptr<asset_command> pending_command{nullptr};
    /* Guarded by client_lock. Refreshed by refreshSmmSettings() (poller
     * thread, plus once at identify time); read by sendSMMSettings() so the
     * main loop never performs a synchronous DB read. */
    std::shared_ptr<smm_settings> cached_smm_settings{nullptr};
    std::string name{};
    std::mutex client_lock{};
    std::list<std::shared_ptr<fss_client_rtt>> outstanding_rtt_requests{};
    /* Responses that arrived before sendRTTRequest() could push its matching
     * outstanding entry (todo/35 — see the rtt_response handler and
     * sendRTTRequest() in client_session.cpp). Bounded FIFO: legitimate
     * traffic never needs more than one slot at a time; the cap defends
     * against a peer spamming bogus ids growing this list unboundedly. */
    static constexpr size_t max_stray_rtt_responses = 4;
    std::list<std::shared_ptr<fss_client_rtt>> stray_rtt_responses{};
    auto getName() -> std::string;
    /* Fold one RTT round trip into the smoothed client↔server clock offset that
     * feeds the staleness gate (todo/17 item 3). client_timestamp is the peer's
     * wall clock from the response (0 = not reported); rtt_ms is the measured
     * round-trip duration, which bounds the estimate's error; recv_wall is the
     * server wall clock captured at receive, adjacent to the monotonic receive
     * time rtt_ms is derived from, so the two refer to the same instant and an
     * NTP step cannot land between them. The client timestamp is trusted data
     * from an authenticated peer — this is a healthy-link correction, not an
     * adversarial defence (see todo/17). Recv thread only. */
    void updateClockOffset(uint64_t client_timestamp, uint64_t rtt_ms, uint64_t recv_wall);
    /* Log the edges of this session's GPS-fix state (todo/76). Every report is
     * recorded to the database regardless; this only decides what reaches the
     * log, which is why it is edge-triggered and the storage is not. Recv
     * thread only. */
    void logGpsFixState(bool gps_fix_valid, bool have_coords);
    uint64_t last_command_send_ts{0};
    uint64_t last_command_dbid{0};
    /* The command row this session has already recorded a dispatch write for
     * (todo/68). Deliberately separate from last_command_dbid, which
     * sendCommand()'s mark_handled also sets on a permanent validation
     * refusal — a command that was never sent must not count as dispatched.
     * Guarded by client_lock. */
    uint64_t last_dispatch_write_dbid{0};
    /* The command row this connection's aircraft has reported a terminal
     * outcome for, which ends its redelivery (todo/68). 0 = none.
     *
     * Scoped to the connection, and that is load-bearing rather than
     * incidental: an FMU has no persistent storage, so the server must never
     * assume an aircraft still holds a command it acked on an earlier
     * connection. A session is built per accepted connection and never reused,
     * so an aircraft that restarts arrives with this at 0, is dispatched at
     * identify time, and is resent to until *that* connection acks terminally.
     * Storing it on the command row instead would wrongly survive the
     * reconnect. See docs/decisions/68-command-redelivery-and-ack-keying.md.
     * Guarded by client_lock. */
    uint64_t terminally_acked_dbid{0};
    /* Record a terminal ack for command_dbid, ending its resend on this
     * connection. Takes client_lock. */
    void markCommandTerminallyAcked(uint64_t command_dbid);
    /* Guarded by client_lock. Maps a dispatch id (the per-connection message id
     * sendMsg() stamped onto a dispatched command) to the command row it
     * delivered, so an ack echoing that id names an exact row (todo/68). The
     * wire id alone cannot: it restarts at 0 on every connection and is unique
     * to neither the asset nor the session. Translating here rather than in SQL
     * also means an ack for a dispatch this session never sent is dropped
     * instead of reaching the database as a plausible-looking update.
     *
     * A server-side session is built per accepted connection and never reused,
     * so this starts empty for every new connection by construction. Bounded
     * FIFO, like stray_rtt_responses above: a session holds one entry per
     * delivery, and the cap stops a peer that never acks from growing it
     * without limit. Oldest-first eviction is the right direction — an
     * unacked dispatch that old has been superseded by the ones behind it. */
    static constexpr size_t max_tracked_dispatches = 16;
    std::list<std::pair<uint64_t, uint64_t>> dispatched_commands{};
    /* Remember that dispatch_id delivered command_dbid, evicting the oldest
     * entry once the cap is reached. Returns true when this is the first
     * delivery of that command on this connection, i.e. when a dispatch write
     * is due; a resend returns false. Takes client_lock. */
    auto recordDispatchedCommand(uint64_t dispatch_id, uint64_t command_dbid) -> bool;
    /* The command row dispatch_id delivered, or 0 if this session never
     * dispatched it (or it has aged out of the FIFO). Takes client_lock. */
    auto lookupDispatchedCommand(uint64_t dispatch_id) -> uint64_t;
    bool liveness_active{false};
    uint64_t last_rtt_response_time{0};
    uint64_t client_timeout_ms{30000};
    bool activated{false};
    uint64_t activated_ms{0};
    uint64_t identify_timeout_ms{30000};
    bool identify_timeout_logged{false};
    IDatabase *dbc;
    std::shared_ptr<db_write_queue> writer;
    fss_client_handler *client_handler;
    std::shared_ptr<IClock> clock{std::make_shared<MonotonicClock>()};
    rate_limiter msg_rate{100, 20};
    uint64_t rate_limit_rejects{0};
    uint64_t last_rate_limit_log_ms{0};
    /* todo/37: command_ack gets its own bucket rather than sharing msg_rate
     * (which would make it a bulk-telemetry casualty) or being exempted
     * outright (unlike rtt_response, an ack is not naturally bounded — see
     * the comment in processMessage()). Sized well above any legitimate
     * cadence (a command produces at most two acks) while still capping a
     * flood of unvalidated acks into the shared db_write_queue. */
    rate_limiter command_ack_rate{10, 5};
    uint64_t ack_rate_limit_rejects{0};
    uint64_t last_ack_rate_limit_log_ms{0};
    /* Position reports in the CURRENT no-fix run — reset when the fix returns,
     * so the restore log can name how long the outage ran. Touched only on the
     * recv thread (processMessage), used to throttle the warning. */
    uint64_t no_fix_reports{0};
    /* Last GPS-fix state seen this session, and whether any has been seen yet.
     * Session-scoped by design: a reconnect mid-outage genuinely has no prior
     * state and re-logs the loss, which is information rather than a duplicate.
     * Drives logging only — every report is recorded either way. Recv thread
     * only. */
    bool gps_fix_state_known{false};
    bool gps_fix_valid{true};
    /* Position staleness window in ms (0 disables the check). A report whose
     * timestamp is further than this from the (offset-corrected) server clock
     * is discarded. Configurable via server.json position_staleness_ms. */
    uint64_t position_staleness_ms{default_position_staleness_ms};
    /* Estimate of (client clock - server clock) in ms: positive if the client
     * runs ahead. Used only to offset-correct the staleness gate, never to
     * rewrite stored timestamps. Measured from RTT responses that carry the
     * client's clock (todo/17 item 3, via updateClockOffset); stays 0 — making
     * the gate a plain symmetric window — until a peer that negotiated the
     * capability reports its clock. */
    int64_t client_clock_offset_ms{0};
    /* False until the first usable RTT clock-offset sample arrives; gates
     * whether client_clock_offset_ms is a measurement or the unmeasured-0
     * default, so the first sample seeds the smoother directly instead of being
     * averaged against a meaningless 0. Recv thread only. */
    bool clock_offset_measured{false};
    /* Consecutive staleness discards; reset by the first in-window report.
     * Drives WARN->ERROR escalation so a skewed client is unmistakable without
     * a per-message WARN drip. Recv thread only. */
    uint64_t staleness_discards{0};
    /* Cumulative duplicate protocol-version messages seen this session
     * (never reset); touched only on the recv thread (processMessage), used
     * to throttle the warning. */
    uint64_t duplicate_version_count{0};
    /* Per-client outbound writer (todo/21). The server main loop schedules
     * *what* to send by setting these pending flags; the worker thread does the
     * actual blocking socket write, so one black-holed peer can only stall its
     * own writer, never command dispatch or RTT handling for other clients.
     * Flags coalesce — at most one of each kind is ever queued — which keeps the
     * queue bounded and lets a command never sit behind stale periodic work.
     * All four bools and the thread handle are guarded by outbound_lock. */
    std::mutex outbound_lock{};
    std::condition_variable outbound_cv{};
    bool outbound_stopping{false};
    bool outbound_started{false};
    bool out_command_pending{false};
    bool out_rtt_pending{false};
    bool out_smm_pending{false};
    /* Broadcasts routed onto this worker too (todo/36), so a black-holed peer
     * can only stall its own broadcast delivery, never the main loop (server
     * list, every 15s) or another client's recv thread (position relay).
     * Coalescing: only the latest server-list matters, so a new one simply
     * replaces any not-yet-sent previous one. Guarded by outbound_lock. */
    std::shared_ptr<const transport::buf_len> pending_server_list_broadcast{nullptr};
    /* Bounded, drop-oldest (todo/36): position relay is loss-tolerant
     * telemetry (mirrors the transport queue's drop policy and todo/20's
     * command/telemetry distinction), so under sustained backlog the oldest
     * queued report is dropped rather than growing unboundedly. Guarded by
     * outbound_lock. */
    static constexpr size_t max_pending_position_relay = 8;
    std::deque<std::shared_ptr<const transport::buf_len>> pending_position_relay{};
    /* Cumulative reports dropped for this client by the cap above (never
     * reset); guarded by outbound_lock alongside the queue it counts.
     * Exposed so an operator (or a future metrics hook) can see the loss
     * rather than it being silent, mirroring fss_connection::
     * getDroppedMessages() for the transport-level inbound queue. */
    uint64_t position_relay_dropped{0};
    std::thread outbound_worker{};
    /* What the worker should do this wake-up. Returned by waitForOutboundWork so
     * the worker performs the (blocking) sends with no lock held. */
    struct outbound_work {
        bool stop{false};
        bool command{false};
        bool rtt{false};
        bool smm{false};
        std::shared_ptr<const transport::buf_len> server_list_broadcast{nullptr};
        std::vector<std::shared_ptr<const transport::buf_len>> position_relay{};
    };
    /* Block until there is work or a stop request, then atomically take and clear
     * the pending flags. */
    auto waitForOutboundWork() -> outbound_work;
    /* The worker loop: waits for work, then performs the blocking send(s) off the
     * main loop. */
    void outboundWorkerRun();
    /* Idempotent: set the stop flag and wake the worker. The single place that
     * owns the stop signal, so disconnect() and stopOutboundWorker() cannot
     * drift apart. */
    void requestOutboundStop();
    /* Idempotent: request the stop (above) and join the worker. Safe to call more
     * than once and from any thread other than the worker itself. The caller must
     * have already closed the connection's fd if the worker might be blocked in
     * a socket send, otherwise the join can hang for the send timeout. */
    void stopOutboundWorker();
public:
    fss_client(std::shared_ptr<transport::fss_connection> conn, IDatabase *t_dbc,
               std::shared_ptr<db_write_queue> t_writer, fss_client_handler *t_handler);
    fss_client(fss_client &) = delete;
    fss_client(fss_client &&) = delete;
    auto operator=(fss_client &) -> fss_client & = delete;
    auto operator=(fss_client &&) -> fss_client & = delete;
    ~fss_client() override;
    /* Wire this client as the connection's message handler. Call only after
     * per-client config (timeout, rate limits) is set; see the constructor. */
    void activate();
    /* Stops the per-client outbound writer thread and the connection (todo/21).
     * Overrides fss_message_cb::disconnect so a stalled writer is unblocked and
     * joined before the connection is torn down. */
    void disconnect() override;
    void processMessage(std::shared_ptr<transport::fss_message> message) override;
    void sendRTTRequest(const std::shared_ptr<transport::fss_message_rtt_request> &rtt_req);
    /* Synchronous DB read; called from the command poller thread (and once
     * from the recv thread at identify time) — never from the main loop. */
    void refreshSmmSettings();
    /* Sends the cached settings only; no DB access. */
    void sendSMMSettings();
    void sendCommand();
    /* Schedule a command send on this client's outbound worker thread instead of
     * sending inline (todo/21). Returns immediately; the worker performs the
     * blocking write. The server main loop uses this so a stalled peer cannot
     * delay command dispatch to other clients. No-op once disconnecting, or if
     * the worker was never started (activate() starts it). */
    void queueCommandSend();
    /* Schedule an RTT request on this client's outbound worker thread (todo/21).
     * The worker builds a fresh rtt_request per client — the request instance is
     * never shared across connections, so the per-connection id stamp cannot
     * race (the C8 invariant). Returns immediately; no-op once disconnecting. */
    void queueRTTRequest();
    /* Schedule a cached-SMM-settings send on this client's outbound worker
     * thread (todo/21). Returns immediately; no-op once disconnecting. */
    void queueSMMSettings();
    /* Schedule a server-list broadcast on this client's outbound worker thread
     * (todo/36) instead of sending inline, so a black-holed peer cannot stall
     * the main loop's periodic broadcast for other clients. Coalescing: a new
     * call simply replaces any not-yet-sent previous one. `packed` is a frame
     * already produced by getPacked() and may be shared read-only across every
     * recipient — the worker copies it and stamps the id per connection
     * (fss_connection::sendPacked, todo/55). Returns immediately; no-op once
     * disconnecting. */
    void queueServerListBroadcast(std::shared_ptr<const transport::buf_len> packed);
    /* Schedule a relayed position report on this client's outbound worker
     * thread (todo/36) instead of sending inline from the reporting client's
     * recv thread, so one black-holed peer cannot stall every other client's
     * telemetry relay. Bounded, drop-oldest: position relay is loss-tolerant.
     * `packed` is a getPacked() frame shared read-only across recipients; the
     * worker copies it and stamps the id per connection (sendPacked, todo/55).
     * Returns immediately; no-op once disconnecting. */
    void queuePositionRelay(std::shared_ptr<const transport::buf_len> packed);
    /* Cumulative position reports dropped from this client's relay queue by
     * the bounded-cap policy above. 0 unless the queue has ever overflowed. */
    auto getPositionRelayDropped() -> uint64_t;
    auto isAircraft() -> bool;
    auto getCachedAssetId() -> uint64_t { return this->cached_asset_id.load(); }
    /* The smoothed client↔server clock offset (ms; positive = client ahead)
     * currently feeding the staleness gate. 0 until measured. Exposed for tests. */
    auto getClockOffsetMs() -> int64_t { return this->client_clock_offset_ms; }
    void setPendingCommand(std::shared_ptr<asset_command> cmd);
    void setClock(std::shared_ptr<IClock> t_clock);
    void setTimeoutMs(uint64_t ms);
    void setIdentifyTimeoutMs(uint64_t ms);
    void setStalenessMs(uint64_t ms);
    void setRateLimits(uint64_t capacity, uint64_t refill_per_s); // must be called before any messages are processed
    auto isTimedOut() -> bool;
};
} // namespace server
} // namespace flight_safety_system
