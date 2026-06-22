#pragma once

#include "fss.hpp"
#include "fss-transport.hpp"
#include "db-write-queue.hpp"
#include "rate-limiter.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <stdexcept>
#include <string>
#include <list>
#include <mutex>
#include <thread>
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

/* Thrown by a database read that could only return a partial, misleading
 * result. getActiveServers raises it when the cursor is cut short by a
 * mid-iteration error so the caller discards the truncated list rather than
 * mistaking it for the complete set of active servers. */
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
    virtual auto getAssetId(const std::string &name) -> uint64_t = 0;
    virtual void recordPosition(uint64_t asset_id, double latitude, double longitude, uint32_t altitude) = 0;
    virtual void recordRtt(uint64_t asset_id, uint64_t rtt_ms) = 0;
    virtual void recordStatus(uint64_t asset_id, uint8_t bat_percent, uint32_t bat_mah_used, double bat_voltage) = 0;
    virtual void recordSearchStatus(uint64_t asset_id, uint64_t search_id, uint64_t completed, uint64_t total) = 0;
    /* Records the dispatch id (the per-connection message id the server stamped
     * on the command) against the command row, so a later ack can be matched to
     * this specific command. */
    virtual void recordCommandDispatch(uint64_t command_dbid, uint64_t dispatch_id) = 0;
    /* Stores a command ack against the row whose (asset_id, dispatch_id) matches;
     * never regresses an already-terminal outcome. dispatch_id is only
     * per-connection unique, so asset_id scopes the match to the acking asset.
     * ack_state/ack_reason are the fss_command_ack_outcome/fss_command_ack_reason
     * ints. */
    virtual void recordCommandAck(uint64_t asset_id, uint64_t dispatch_id, uint8_t ack_state, uint64_t ack_timestamp,
                                  uint8_t ack_reason) = 0;
    virtual auto getCommand(uint64_t asset_id) -> std::shared_ptr<asset_command> = 0;
    virtual auto getActiveServers() -> std::vector<fss_server_details> = 0;
    virtual auto getSmmSettings(uint64_t asset_id) -> std::shared_ptr<smm_settings> = 0;
    virtual auto isConnected() const -> bool = 0;
    virtual void tryReconnectIfNeeded() = 0;
};

class db_connection : public IDatabase {
private:
    /* Two independent ECPG connections so a stalled telemetry write cannot
     * block a command/config read. Each connection is guarded by its own
     * mutex and therefore only ever used by one thread at a time — the
     * thread-safe usage pattern ECPG documents. read_lock covers getAssetId,
     * getCommand, getActiveServers and getSmmSettings; write_lock covers the
     * record* telemetry inserts. */
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
    std::string pass_;
    std::string db_;
    auto connectOne(const char *conn_name) -> bool;
    void reconnectOne(const char *conn_name, std::atomic<bool> &connected_flag);
public:
    /* ECPG connection names, exposed so tests can target a specific
     * connection (e.g. force one closed) without duplicating the literals. */
    static constexpr const char *read_conn_name = "fss_read";
    static constexpr const char *write_conn_name = "fss_write";
    db_connection(std::string host, int port, std::string user, std::string pass, std::string db);
    db_connection(db_connection &) = delete;
    db_connection(db_connection &&) = delete;
    auto operator=(db_connection &) -> db_connection & = delete;
    auto operator=(db_connection &&) -> db_connection & = delete;
    ~db_connection() override;
    auto getAssetId(const std::string &name) -> uint64_t override;
    void recordPosition(uint64_t asset_id, double latitude, double longitude, uint32_t altitude) override;
    void recordRtt(uint64_t asset_id, uint64_t rtt_ms) override;
    void recordStatus(uint64_t asset_id, uint8_t bat_percent, uint32_t bat_mah_used, double bat_voltage) override;
    void recordSearchStatus(uint64_t asset_id, uint64_t search_id, uint64_t completed, uint64_t total) override;
    void recordCommandDispatch(uint64_t command_dbid, uint64_t dispatch_id) override;
    void recordCommandAck(uint64_t asset_id, uint64_t dispatch_id, uint8_t ack_state, uint64_t ack_timestamp,
                          uint8_t ack_reason) override;
    auto getCommand(uint64_t asset_id) -> std::shared_ptr<asset_command> override;
    auto getActiveServers() -> std::vector<fss_server_details> override;
    auto getSmmSettings(uint64_t asset_id) -> std::shared_ptr<smm_settings> override;
    auto isConnected() const -> bool override;
    void tryReconnectIfNeeded() override;
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

class fss_client_handler {
public:
    virtual ~fss_client_handler() = default;
    virtual void clientDisconnected(fss_client *client) = 0;
    /* Sends msg to every client (bar `except`). Implementations must iterate
     * the connections sequentially: fss_connection::sendMsg stamps a
     * per-connection id into msg, so fanning one instance out concurrently
     * would race that write (todo/12 C8). */
    virtual void broadcastMsg(const std::shared_ptr<transport::fss_message> &msg, fss_client *except = nullptr) = 0;
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
    uint64_t last_command_send_ts{0};
    uint64_t last_command_dbid{0};
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
    /* Cumulative position reports discarded for having no GPS fix (NaN
     * coordinates) this session; touched only on the recv thread
     * (processMessage), used to throttle the warning. */
    uint64_t no_fix_reports{0};
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
    /* Cumulative out-of-order / duplicate (v2 sequence) messages seen this
     * session (never reset); touched only on the recv thread (processMessage),
     * used to throttle the warning so a peer streaming wrong sequence numbers
     * cannot flood the log (the seq check runs before the rate limiter). */
    uint64_t out_of_order_count{0};
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
    std::thread outbound_worker{};
    /* What the worker should do this wake-up. Returned by waitForOutboundWork so
     * the worker performs the (blocking) sends with no lock held. */
    struct outbound_work {
        bool stop{false};
        bool command{false};
        bool rtt{false};
        bool smm{false};
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
