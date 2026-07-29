#include "fss-transport.hpp"
#include "fss.hpp"
#include "fss-log.hpp"
#include "fss-server.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

namespace fss = flight_safety_system;

static auto is_valid_coordinate(double latitude, double longitude) -> bool
{
    return std::isfinite(latitude) && std::isfinite(longitude) && latitude >= -90.0 && latitude <= 90.0 &&
           longitude >= -180.0 && longitude <= 180.0;
}

constexpr uint64_t sec_to_msec = 1000;
constexpr uint64_t rtt_retry_interval = 10 * sec_to_msec;
/* Staleness-discard escalation: first discard logs WARN; the Nth consecutive
 * (and every Mth after) logs an ERROR naming the suspected clock skew, so a
 * persistently skewed client is unmistakable without a per-message WARN drip. */
constexpr uint64_t staleness_escalation_threshold = 10;
constexpr uint64_t staleness_escalation_interval = 100;
/* RTT clock-offset smoothing (todo/17 item 3). A sample's error is bounded by
 * half the round trip, so a link slower than this yields an estimate too coarse
 * to trust for the staleness window — drop it rather than poison the average. */
constexpr uint64_t max_rtt_for_clock_offset_ms = 5000;
/* EWMA weight: each new sample moves the smoothed offset by 1/N of the gap, so
 * jitter is damped while genuine drift is still tracked. */
constexpr int64_t clock_offset_smoothing_divisor = 4;

fss::server::smm_settings::smm_settings(std::string t_address, fss::secure_string t_username,
                                        fss::secure_string t_password)
    : address(std::move(t_address)), username(std::move(t_username)), password(std::move(t_password))
{
}

auto fss::server::smm_settings::getAddress() -> std::string
{
    return this->address;
}
auto fss::server::smm_settings::getUsername() -> const fss::secure_string &
{
    return this->username;
}
auto fss::server::smm_settings::getPassword() -> const fss::secure_string &
{
    return this->password;
}

fss::server::fss_server_details::fss_server_details(std::string t_address, uint16_t t_port)
    : address(std::move(t_address)), port(t_port)
{
}

auto fss::server::fss_server_details::getAddress() -> std::string
{
    return this->address;
}
auto fss::server::fss_server_details::getPort() -> uint16_t
{
    return this->port;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
fss::server::asset_command::asset_command(uint64_t t_dbid, uint64_t t_timestamp, const std::string &t_cmd,
                                          double t_latitude, double t_longitude, uint32_t t_altitude,
                                          bool t_altitude_valid)
    : dbid(t_dbid), timestamp(t_timestamp), latitude(t_latitude), longitude(t_longitude), altitude(t_altitude),
      altitude_valid(t_altitude_valid)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    if (t_cmd == "RTL")
    {
        this->command = transport::asset_command_rtl;
    }
    else if (t_cmd == "HOLD")
    {
        this->command = transport::asset_command_hold;
    }
    else if (t_cmd == "GOTO")
    {
        this->command = transport::asset_command_goto;
    }
    else if (t_cmd == "RON")
    {
        this->command = transport::asset_command_resume;
    }
    else if (t_cmd == "DISARM")
    {
        this->command = transport::asset_command_disarm;
    }
    else if (t_cmd == "ALT")
    {
        this->command = transport::asset_command_altitude;
    }
    else if (t_cmd == "TERM")
    {
        this->command = transport::asset_command_terminate;
    }
    else if (t_cmd == "MAN")
    {
        this->command = transport::asset_command_manual;
    }
    else
    {
        FSS_LOG_ERROR("server", "Unrecognised command string from DB: '" << t_cmd << "' (dbid=" << t_dbid << ")");
    }
}

auto fss::server::asset_command::getDBId() -> uint64_t
{
    return this->dbid;
}
auto fss::server::asset_command::getTimeStamp() -> uint64_t
{
    return this->timestamp;
}
auto fss::server::asset_command::getCommand() -> fss::transport::fss_asset_command
{
    return this->command;
}
auto fss::server::asset_command::getLatitude() -> double
{
    return this->latitude;
}
auto fss::server::asset_command::getLongitude() -> double
{
    return this->longitude;
}
auto fss::server::asset_command::getAltitude() -> uint32_t
{
    return this->altitude;
}
auto fss::server::asset_command::isAltitudeValid() -> bool
{
    return this->altitude_valid;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
fss::server::fss_client_rtt::fss_client_rtt(uint64_t t_timestamp, uint64_t t_reqid)
    : timestamp(t_timestamp), reqid(t_reqid)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
}

auto fss::server::fss_client_rtt::getTimeStamp() -> uint64_t
{
    return this->timestamp;
}
auto fss::server::fss_client_rtt::getRequestId() -> uint64_t
{
    return this->reqid;
}

fss::server::fss_client::fss_client(std::shared_ptr<fss::transport::fss_connection> t_conn, IDatabase *t_dbc,
                                    std::shared_ptr<db_write_queue> t_writer, fss_client_handler *t_handler)
    : fss_message_cb(std::move(t_conn)), dbc(t_dbc), writer(std::move(t_writer)), client_handler(t_handler)
{
    /* The connection's recv thread may already be running (it is started by
     * fss_connection_server::create before this client exists). The handler
     * is wired separately, via activate(), only after per-client config
     * (timeout, rate limits) has been applied — otherwise the recv thread
     * could deliver a message and touch msg_rate while it is being
     * reconfigured. Until then, inbound messages queue on the connection. */
}

void fss::server::fss_client::activate()
{
    {
        std::scoped_lock guard(this->client_lock);
        this->activated_ms = this->clock->now_ms();
        this->activated = true;
    }
    {
        /* Start the outbound writer thread (todo/21) before wiring the handler,
         * so it is ready for any send scheduled as a side effect of the first
         * flushed message. */
        std::scoped_lock guard(this->outbound_lock);
        if (!this->outbound_started && !this->outbound_stopping)
        {
            this->outbound_started = true;
            this->outbound_worker = std::thread(&fss_client::outboundWorkerRun, this);
        }
    }
    this->getConnection()->setHandler(this);
}

auto fss::server::fss_client::waitForOutboundWork() -> fss::server::fss_client::outbound_work
{
    std::unique_lock<std::mutex> lock(this->outbound_lock);
    this->outbound_cv.wait(lock, [this]() -> bool {
        return this->outbound_stopping || this->out_command_pending || this->out_rtt_pending || this->out_smm_pending ||
               this->pending_server_list_broadcast != nullptr || !this->pending_position_relay.empty();
    });
    outbound_work work;
    if (this->outbound_stopping)
    {
        work.stop = true;
        return work;
    }
    work.command = this->out_command_pending;
    work.rtt = this->out_rtt_pending;
    work.smm = this->out_smm_pending;
    this->out_command_pending = false;
    this->out_rtt_pending = false;
    this->out_smm_pending = false;
    work.server_list_broadcast = std::move(this->pending_server_list_broadcast);
    this->pending_server_list_broadcast = nullptr;
    work.position_relay.assign(std::make_move_iterator(this->pending_position_relay.begin()),
                               std::make_move_iterator(this->pending_position_relay.end()));
    this->pending_position_relay.clear();
    return work;
}

void fss::server::fss_client::outboundWorkerRun()
{
    for (;;)
    {
        auto work = this->waitForOutboundWork();
        /* Periodic sends are best-effort: anything not yet written is
         * re-scheduled on the next tick if the client survives, and a
         * disconnecting client has nothing left to say. Exit promptly on stop so
         * a join never waits on a send. */
        if (work.stop)
        {
            return;
        }
        /* Commands are the highest priority — send them before periodic RTT,
         * SMM settings, and broadcasts. */
        if (work.command)
        {
            this->sendCommand();
        }
        if (work.rtt)
        {
            /* A fresh request per client: never a shared instance (see the C8
             * invariant on fss_connection::sendMsg). */
            this->sendRTTRequest(std::make_shared<fss::transport::fss_message_rtt_request>());
        }
        if (work.smm)
        {
            this->sendSMMSettings();
        }
        if (work.server_list_broadcast != nullptr)
        {
            /* Broadcast frames are pre-packed and shared read-only across
             * recipients; sendPacked copies and stamps this connection's id
             * (todo/55) rather than re-packing a decoded clone. */
            this->getConnection()->sendPacked(work.server_list_broadcast);
        }
        for (const auto &relay_msg : work.position_relay)
        {
            this->getConnection()->sendPacked(relay_msg);
        }
    }
}

void fss::server::fss_client::queueCommandSend()
{
    {
        std::scoped_lock guard(this->outbound_lock);
        if (this->outbound_stopping)
        {
            return;
        }
        this->out_command_pending = true;
    }
    this->outbound_cv.notify_one();
}

void fss::server::fss_client::queueRTTRequest()
{
    {
        std::scoped_lock guard(this->outbound_lock);
        if (this->outbound_stopping)
        {
            return;
        }
        this->out_rtt_pending = true;
    }
    this->outbound_cv.notify_one();
}

void fss::server::fss_client::queueSMMSettings()
{
    {
        std::scoped_lock guard(this->outbound_lock);
        if (this->outbound_stopping)
        {
            return;
        }
        this->out_smm_pending = true;
    }
    this->outbound_cv.notify_one();
}

void fss::server::fss_client::queueServerListBroadcast(std::shared_ptr<const fss::transport::buf_len> packed)
{
    {
        std::scoped_lock guard(this->outbound_lock);
        if (this->outbound_stopping)
        {
            return;
        }
        this->pending_server_list_broadcast = std::move(packed);
    }
    this->outbound_cv.notify_one();
}

void fss::server::fss_client::queuePositionRelay(std::shared_ptr<const fss::transport::buf_len> packed)
{
    uint64_t dropped = 0;
    {
        std::scoped_lock guard(this->outbound_lock);
        if (this->outbound_stopping)
        {
            return;
        }
        if (this->pending_position_relay.size() >= max_pending_position_relay)
        {
            this->pending_position_relay.pop_front();
            dropped = ++this->position_relay_dropped;
        }
        this->pending_position_relay.push_back(std::move(packed));
    }
    this->outbound_cv.notify_one();
    /* Logged outside outbound_lock: getName() takes client_lock, and name is
     * guarded by that lock, not outbound_lock. */
    if (dropped == 1 || (dropped != 0 && dropped % 100 == 0))
    {
        FSS_LOG_WARN("server", "Position relay queue full for " << this->getName()
                                                                << ", dropping oldest. Total dropped: " << dropped);
    }
}

auto fss::server::fss_client::getPositionRelayDropped() -> uint64_t
{
    std::scoped_lock guard(this->outbound_lock);
    return this->position_relay_dropped;
}

void fss::server::fss_client::requestOutboundStop()
{
    {
        std::scoped_lock guard(this->outbound_lock);
        if (this->outbound_stopping)
        {
            return;
        }
        this->outbound_stopping = true;
    }
    this->outbound_cv.notify_all();
}

void fss::server::fss_client::stopOutboundWorker()
{
    this->requestOutboundStop();
    if (!this->outbound_worker.joinable())
    {
        return;
    }
    if (this->outbound_worker.get_id() == std::this_thread::get_id())
    {
        /* The worker must never join itself; detach so it can finish. This is
         * not expected (the worker only performs sends, never disconnects), but
         * mirrors the recv-thread guard in fss_connection::disconnect(). */
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
        FSS_LOG_ERROR("server", "outbound_worker.join() failed, detaching: " << e.what());
        this->outbound_worker.detach();
        this->outbound_worker = std::thread();
    }
}

void fss::server::fss_client::disconnect()
{
    /* Order matters (todo/21, todo/52): signal the worker to stop, then shut
     * the socket down — not close it — so a worker blocked in send() returns
     * (EPIPE) while the descriptor number stays reserved, then join the
     * worker, and only then run the connection's disconnect(), which joins the
     * recv thread and performs the deferred close. Closing any earlier would
     * free the fd number for reuse while the worker may still be about to pass
     * its stale value to send() — I/O into an unrelated session (todo/52). All
     * of this runs before the base class clears the connection, and the worker
     * only ever sees a non-null connection (its sends fail fast once the fd
     * reads -1), so it can never dereference a cleared connection. */
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
    fss_message_cb::disconnect(); // a second disconnect() on the connection here is a safe no-op
}

fss::server::fss_client::~fss_client()
{
    /* Single teardown entry point: disconnect() stops the worker (shutting the
     * socket down first so a blocked send returns) and clears the connection.
     * It is idempotent, so this is safe whether or not disconnect() was already
     * called. */
    fss_client::disconnect();
}

auto fss::server::fss_client::isAircraft() -> bool
{
    return this->aircraft;
}

auto fss::server::fss_client::getName() -> std::string
{
    std::scoped_lock guard(this->client_lock);
    return this->name;
}

void fss::server::fss_client::setPendingCommand(std::shared_ptr<fss::server::asset_command> cmd)
{
    std::scoped_lock guard(this->client_lock);
    this->pending_command = std::move(cmd);
}

void fss::server::fss_client::sendCommand()
{
    /* todo/35: the dbid/resend-window checks and validation run under
     * client_lock, but the send itself must not — a black-holed peer can
     * block sendMsg() for up to TCP_USER_TIMEOUT, and isTimedOut() (called
     * every main-loop tick, for every client) takes the same lock. Holding it
     * across the send would freeze the whole fleet's command cadence behind
     * one stuck peer. */
    std::shared_ptr<fss::transport::fss_message_asset_command> msg = nullptr;
    uint64_t ts = 0;
    uint64_t dbid = 0;
    std::string client_name;
    uint64_t prev_send_ts = 0;
    uint64_t prev_dbid = 0;
    {
        std::scoped_lock guard(this->client_lock);
        if (this->cached_asset_id.load() == 0)
        {
            return;
        }
        ts = this->clock->now_ms();
        auto ac = this->pending_command;
        if (ac == nullptr)
        {
            return;
        }
        client_name = this->name;
        constexpr int timeout_time = 10 * sec_to_msec;
        dbid = ac->getDBId();
        bool is_new_command = dbid != this->last_command_dbid;
        bool resend_window_expired = ts > (this->last_command_send_ts + timeout_time);
        if (!is_new_command && !resend_window_expired)
        {
            /* Same command we last handled and still inside the resend window. */
            return;
        }
        /* Mark a command as handled for the current resend window: applied on a
         * permanent validation rejection below (which can never succeed, so
         * re-evaluating it every tick would only spam the log). Deliberately
         * NOT applied on a transient send failure, which must stay eligible
         * for retry on the next send tick. */
        auto mark_handled = [this](uint64_t handled_ts, uint64_t handled_dbid) -> void {
            this->last_command_send_ts = handled_ts;
            this->last_command_dbid = handled_dbid;
        };
        auto command = ac->getCommand();
        if (command == fss::transport::asset_command_unknown)
        {
            mark_handled(ts, dbid);
            FSS_LOG_ERROR("server",
                          "Refusing to dispatch unknown command type to " << client_name << " (dbid=" << dbid << ")");
            return;
        }
        if (command == fss::transport::asset_command_goto &&
            !is_valid_coordinate(ac->getLatitude(), ac->getLongitude()))
        {
            mark_handled(ts, dbid);
            FSS_LOG_ERROR("server", "Refusing to dispatch GOTO command with invalid coordinates to "
                                        << client_name << " (dbid=" << dbid << ", lat=" << ac->getLatitude()
                                        << ", lon=" << ac->getLongitude() << ")");
            return;
        }
        if (command == fss::transport::asset_command_altitude && !ac->isAltitudeValid())
        {
            /* A NULL altitude must not dispatch as 0 — that is a
             * descend-to-ground instruction. */
            mark_handled(ts, dbid);
            FSS_LOG_ERROR("server", "Refusing to dispatch ALT command with NULL altitude to "
                                        << client_name << " (dbid=" << dbid << ")");
            return;
        }
        switch (command)
        {
            case fss::transport::asset_command_goto:
                msg = std::make_shared<fss::transport::fss_message_asset_command>(
                    command, ac->getTimeStamp(), ac->getLatitude(), ac->getLongitude());
                break;
            case fss::transport::asset_command_altitude:
                msg = std::make_shared<fss::transport::fss_message_asset_command>(command, ac->getTimeStamp(),
                                                                                  ac->getAltitude());
                break;
            default:
                msg = std::make_shared<fss::transport::fss_message_asset_command>(command, ac->getTimeStamp());
                break;
        }
        /* Stamp this server's identity for the operator action — the command
         * DB row id (todo/49) — only when the peer negotiated the capability;
         * otherwise the trailing field is not part of the agreed dialect and
         * the message keeps its legacy wire form. getConnection() only takes
         * its own leaf conn_lock, so holding client_lock here is safe. */
        auto command_conn = this->getConnection();
        if (command_conn != nullptr &&
            (command_conn->getNegotiatedFeatureFlags() & fss::transport::FSS_FEATURE_SERVER_COMMAND_ID) != 0)
        {
            msg->setServerCommandId(dbid);
        }
        /* Claim the resend window now, atomically with the check above, not
         * after the send returns: the recv thread can also call sendCommand()
         * directly (identify handling) while this outbound-worker call is
         * still mid-flight in the blocking send below, and without an
         * immediate claim both calls would see the stale (pre-send)
         * last_command_dbid/ts and dispatch the same command twice. Rolled
         * back on send failure, below, so a transient failure still retries
         * on the very next tick rather than waiting out the resend window. */
        prev_send_ts = this->last_command_send_ts;
        prev_dbid = this->last_command_dbid;
        this->last_command_send_ts = ts;
        this->last_command_dbid = dbid;
    } // client_lock released before the blocking send

    /* Null-safe base-class sendMsg: sendCommand() is called from the identify
     * path, which can lose its connection to a concurrent evict_oldest
     * teardown (see the pinned-connection note in processMessage); a cleared
     * connection reads as a failed send, taking the retry branch below. */
    if (!this->sendMsg(msg))
    {
        /* The socket write failed, so the command never reached the aircraft.
         * Undo the claim above so the command is retried on the next send tick
         * rather than appearing in the DB as dispatched — but only if nobody
         * else has since claimed a newer command; otherwise leave their claim
         * alone. A genuinely dead connection is reaped separately by the recv
         * thread's closed-message path. */
        {
            std::scoped_lock guard(this->client_lock);
            if (this->last_command_dbid == dbid && this->last_command_send_ts == ts)
            {
                this->last_command_dbid = prev_dbid;
                this->last_command_send_ts = prev_send_ts;
            }
        }
        FSS_LOG_WARN("server",
                     "Failed to send command dbid=" << dbid << " to " << client_name << "; will retry on next tick");
        return;
    }
    /* sendMsg stamped the per-connection message id into msg; record it against
     * the command row (cached_asset_id is non-zero here — the early return above
     * guarantees it) so a later ack, which echoes this id as acked_command_id,
     * can be matched back to this specific command. */
    this->writer->enqueue(command_dispatch_write{dbid, msg->getId()});
    FSS_LOG_INFO("server", "dispatched command dbid=" << dbid << " to " << client_name);
}

void fss::server::fss_client::setClock(std::shared_ptr<fss::IClock> t_clock)
{
    if (t_clock == nullptr)
    {
        return;
    }
    this->clock = std::move(t_clock);
}

void fss::server::fss_client::setTimeoutMs(uint64_t ms)
{
    std::scoped_lock guard(this->client_lock);
    this->client_timeout_ms = ms;
}

void fss::server::fss_client::setIdentifyTimeoutMs(uint64_t ms)
{
    std::scoped_lock guard(this->client_lock);
    this->identify_timeout_ms = ms;
}

void fss::server::fss_client::setStalenessMs(uint64_t ms)
{
    std::scoped_lock guard(this->client_lock);
    this->position_staleness_ms = ms;
}

void fss::server::fss_client::setRateLimits(uint64_t capacity, uint64_t refill_per_s)
{
    this->msg_rate = rate_limiter(capacity, refill_per_s);
}

auto fss::server::fss_client::isTimedOut() -> bool
{
    std::scoped_lock guard(this->client_lock);
    if (!this->identified)
    {
        /* Only meaningful once the client has been activated: before that
         * activated_ms is unset, so a caller invoking isTimedOut() early must
         * not see a spurious timeout against an epoch-zero activation time. */
        if (!this->activated)
        {
            return false;
        }
        bool timed_out = (this->clock->now_ms() - this->activated_ms) > this->identify_timeout_ms;
        if (timed_out && !this->identify_timeout_logged)
        {
            this->identify_timeout_logged = true;
            FSS_LOG_WARN("server", "Client did not identify within deadline; pruning");
        }
        return timed_out;
    }
    if (!this->liveness_active)
    {
        return false;
    }
    return (this->clock->now_ms() - this->last_rtt_response_time) > this->client_timeout_ms;
}

void fss::server::fss_client::sendRTTRequest(const std::shared_ptr<fss::transport::fss_message_rtt_request> &rtt_req)
{
    /* todo/35: as in sendCommand(), client_lock must not be held across the
     * blocking send. Unlike sendCommand, the outstanding-request bookkeeping
     * must be keyed by the message id sendMsg() stamps onto rtt_req — and
     * that stamp only happens inside sendMsg() itself (fss_connection::
     * sendMsg, under its own send_lock, before the blocking write), so the
     * real id is not known until the call returns. The entry is therefore
     * pushed immediately after a successful send (todo/22: only a request
     * that actually went out is tracked), not before it.
     *
     * todo/53: the send below now goes through the null-safe base-class
     * sendMsg(), like sendCommand() and sendSMMSettings() — a connection
     * cleared by a concurrent teardown reads as a failed send (handled below
     * by reaping via clientDisconnected) instead of a null-pointer crash. */
    bool timed_out = false;
    {
        std::scoped_lock guard(this->client_lock);
        uint64_t check_now = this->clock->now_ms();
        if (!this->outstanding_rtt_requests.empty())
        {
            if (check_now - this->outstanding_rtt_requests.front()->getTimeStamp() > this->client_timeout_ms)
            {
                timed_out = true;
            }
            else if (check_now - this->outstanding_rtt_requests.back()->getTimeStamp() < rtt_retry_interval)
            {
                return;
            }
        }
    }
    if (timed_out)
    {
        this->client_handler->clientDisconnected(this);
        return;
    }
    uint64_t now = this->clock->now_ms();
    if (!this->sendMsg(rtt_req))
    {
        /* The socket write failed, so the connection is broken. Reap the client
         * now rather than waiting out the liveness timeout against a peer that
         * can never answer (the recv thread's closed-message path would also get
         * here, but disconnecting directly is immediate and idempotent). */
        FSS_LOG_WARN("server", "Failed to send RTT request to " << this->getName() << "; disconnecting");
        this->client_handler->clientDisconnected(this);
        return;
    }
    uint64_t assigned_id = rtt_req->getId();
    std::shared_ptr<fss_client_rtt> stray;
    {
        std::scoped_lock guard(this->client_lock);
        auto found = std::find_if(
            this->stray_rtt_responses.begin(), this->stray_rtt_responses.end(),
            [assigned_id](const auto &candidate) -> bool { return candidate->getRequestId() == assigned_id; });
        if (found != this->stray_rtt_responses.end())
        {
            stray = *found;
        }
        if (stray != nullptr)
        {
            /* The response for this very request already arrived and was
             * recorded as unmatched (see the rtt_response handler) before this
             * function could push its own bookkeeping entry. Consume it now
             * instead of adding an outstanding entry that would otherwise
             * never be answered and eventually force a false liveness
             * disconnect. */
            this->stray_rtt_responses.remove(stray);
            this->last_rtt_response_time = this->clock->now_ms();
        }
        else
        {
            this->outstanding_rtt_requests.push_back(std::make_shared<fss_client_rtt>(now, assigned_id));
        }
    }
    if (stray != nullptr)
    {
        uint64_t asset_id = this->cached_asset_id.load();
        if (asset_id != 0)
        {
            /* stray->getTimeStamp() is when the response was actually
             * received, which predates `now` (when this function resumed
             * after the send) — using it gives a truer round-trip duration
             * than clamping to `now` would. */
            uint64_t rtt_ms = stray->getTimeStamp() > now ? stray->getTimeStamp() - now : 0;
            this->writer->enqueue(rtt_write{asset_id, rtt_ms});
        }
    }
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void fss::server::fss_client::updateClockOffset(uint64_t client_timestamp, uint64_t rtt_ms, uint64_t recv_wall)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    /* 0 means the peer did not report its clock (or its clock genuinely reads
     * the epoch, which we deliberately treat the same — see the field comment
     * in fss-transport.hpp); either way there is nothing usable to fold in. */
    if (client_timestamp == 0)
    {
        return;
    }
    if (rtt_ms > max_rtt_for_clock_offset_ms)
    {
        return;
    }
    /* The request's midpoint in server wall-clock terms is half a round trip
     * before we received the response. rtt_ms is a duration, so subtracting it
     * from recv_wall is sound even though the RTT itself is timed on the
     * monotonic clock; the caller captures recv_wall adjacent to that monotonic
     * receive time so the two refer to the same instant. */
    int64_t midpoint = static_cast<int64_t>(recv_wall) - static_cast<int64_t>(rtt_ms / 2);
    int64_t sample = static_cast<int64_t>(client_timestamp) - midpoint;
    if (!this->clock_offset_measured)
    {
        this->client_clock_offset_ms = sample;
        this->clock_offset_measured = true;
    }
    else
    {
        /* EWMA in integer ms: once the estimate is within
         * clock_offset_smoothing_divisor-1 ms of a sample the delta truncates to
         * 0 and the value rests, which is far finer than any sane staleness
         * window — genuine drift larger than that floor is still tracked. */
        this->client_clock_offset_ms += (sample - this->client_clock_offset_ms) / clock_offset_smoothing_divisor;
    }
}

void fss::server::fss_client::refreshSmmSettings()
{
    uint64_t asset_id = this->cached_asset_id.load();
    if (asset_id == 0)
    {
        return;
    }
    auto smm = this->dbc->getSmmSettings(asset_id);
    std::scoped_lock guard(this->client_lock);
    /* todo/69, interim: getSmmSettings still reports a read failure in-band, as
     * the same nullptr an asset with no settings row produces, so the only way
     * to stop a transient failure wiping a good cache is to refuse the
     * overwrite entirely. That also holds a cache whose row was genuinely
     * deleted, which is the lesser of the two wrongs until the seam reports
     * failure by status and this guard becomes precise. */
    if (smm == nullptr && this->cached_smm_settings != nullptr)
    {
        return;
    }
    this->cached_smm_settings = std::move(smm);
}

void fss::server::fss_client::sendSMMSettings()
{
    std::shared_ptr<smm_settings> smm;
    {
        std::scoped_lock guard(this->client_lock);
        smm = this->cached_smm_settings;
    }
    if (smm != nullptr)
    {
        auto settings_msg = std::make_shared<fss::transport::fss_message_smm_settings>(
            smm->getAddress(), smm->getUsername(), smm->getPassword());
        /* Null-safe base-class sendMsg: also reached from the identify path,
         * same concurrent-teardown hazard as sendCommand(). */
        this->sendMsg(settings_msg);
    }
}

namespace {
/* nullptr when the DB read failed (partial/truncated result): the caller must
 * skip the send or keep its cached list rather than shipping a wrong one. A
 * genuinely empty active-server set is a non-null message with no servers. */
auto getServersListMsg(fss::server::IDatabase *dbc) -> std::shared_ptr<fss::transport::fss_message_server_list>
{
    auto servers = dbc->getActiveServers();
    if (!servers.has_value())
    {
        return nullptr;
    }
    auto server_list = std::make_shared<fss::transport::fss_message_server_list>();
    for (auto &server_details : *servers)
    {
        server_list->addServer(server_details.getAddress(), server_details.getPort());
    }
    return server_list;
}
} // namespace

void fss::server::fss_client::processMessage(std::shared_ptr<fss::transport::fss_message> msg)
{
    if (msg == nullptr)
    {
        return;
    }
#ifdef DEBUG
    std::cout << "Got message " << msg->getType() << std::endl;
#endif
    if (msg->getType() == fss::transport::message_type_closed)
    {
        if (this->identified)
        {
            FSS_LOG_INFO("server", (this->aircraft ? "Aircraft" : "Non-aircraft")
                                       << " client disconnected: " << this->getName());
        }
        this->client_handler->clientDisconnected(this);
        return;
    }
    /* Pin the connection for the remainder of this message (PR #332 TSan
     * crash): a concurrent teardown — e.g. duplicate_identity_evict_oldest
     * severing this client while its identify is still in flight — clears
     * the base class's pointer, and the only production protection is the
     * recv-thread join inside disconnect(), which callers off the recv
     * thread do not get. The local shared_ptr keeps the connection alive
     * and non-null for the whole call; after teardown its fd is closed, so
     * sends simply fail, exactly like any mid-message disconnect. */
    auto active_conn = this->getConnection();
    if (active_conn == nullptr)
    {
        return;
    }
    if (msg->getType() == fss::transport::message_type_version)
    {
        auto version_msg = std::dynamic_pointer_cast<fss::transport::fss_message_version>(msg);
        if (version_msg != nullptr)
        {
            if (this->version_received)
            {
                /* Duplicate handshake from a buggy peer. Honouring it would
                 * re-negotiate the protocol version mid-session and re-anchor
                 * the sequence check; instead advance the sequence (the
                 * duplicate still consumed a message id on the sender) and
                 * drop the message. This path runs before the rate limiter,
                 * so throttle the warning (first, then every 100th, with the
                 * running count) so a peer spamming version messages cannot
                 * flood the log. */
                ++this->duplicate_version_count;
                constexpr uint64_t log_every = 100;
                if (this->duplicate_version_count == 1 || (this->duplicate_version_count % log_every) == 0)
                {
                    FSS_LOG_WARN("server", "Ignoring duplicate protocol version message from "
                                               << this->getName() << " (seq=" << msg->getSeq()
                                               << ", count=" << this->duplicate_version_count << ")");
                }
                uint64_t wanted = this->expected_seq.load();
                if (wanted != 0 && msg->getSeq() == wanted)
                {
                    this->expected_seq.fetch_add(1);
                }
                return;
            }
            uint16_t peer_version = version_msg->getProtocolVersion();
            uint16_t peer_min = version_msg->getMinSupportedVersion();
            if (peer_version < fss::transport::FSS_PROTOCOL_MIN_VERSION ||
                peer_min > fss::transport::FSS_PROTOCOL_VERSION)
            {
                FSS_LOG_ERROR("server", "Client protocol version incompatible: peer="
                                            << peer_version << " peer_min=" << peer_min
                                            << " us=" << fss::transport::FSS_PROTOCOL_VERSION
                                            << " us_min=" << fss::transport::FSS_PROTOCOL_MIN_VERSION);
                this->client_handler->clientDisconnected(this);
                return;
            }
            uint16_t negotiated = std::min(peer_version, fss::transport::FSS_PROTOCOL_VERSION);
            active_conn->setNegotiatedVersion(negotiated);
            active_conn->setNegotiatedFeatureFlags(
                fss::transport::negotiateFeatureFlags(version_msg->getFeatureFlags()));
            this->version_received = true;
            if (negotiated >= 2)
            {
                this->expected_seq.store(msg->getId() + 1);
            }
            auto resp = std::make_shared<fss::transport::fss_message_version>();
            active_conn->sendMsg(resp);
        }
        return;
    }
    /* In-order / duplicate detection (v2+). Each peer numbers the messages it
     * sends with a monotonic per-connection counter; we require the next id to
     * match. This is an application-level integrity check that rejects
     * reordered or duplicated messages within a session — it is NOT a security
     * replay defence. Under its security assumptions TLS already provides
     * replay and reorder protection at the record layer, so this check is not
     * relied upon to stop an attacker. */
    if (active_conn->getNegotiatedVersion() >= 2)
    {
        uint64_t wanted = this->expected_seq.load();
        if (wanted != 0)
        {
            if (msg->getSeq() != wanted)
            {
                /* todo/39: under this check's own stated assumptions (TLS
                 * already rules out genuine reorder/replay at the record
                 * layer), a mismatch on a live connection means either a
                 * broken/misbehaving peer or a local framing bug (e.g. a
                 * consumed-but-never-sent id — see todo/38) — either way,
                 * expected_seq only advances on a match, so silently dropping
                 * would freeze every subsequent message (positions, status,
                 * RTT responses included) in a permanent drop loop until the
                 * 30s liveness timeout eventually reaps the connection.
                 * Disconnect immediately instead, exactly as the identity
                 * case already did — a loud failure and clean reconnect beats
                 * 30s of silently discarded telemetry. Only one such log line
                 * can ever fire per session now (the session ends here), so
                 * no throttling counter is needed. */
                FSS_LOG_WARN("server", "Out-of-order or duplicate message seq=" << msg->getSeq() << " expected="
                                                                                << wanted << " from " << this->getName()
                                                                                << "; disconnecting");
                this->client_handler->clientDisconnected(this);
                return;
            }
            this->expected_seq.fetch_add(1);
        }
    }
    if (!this->identified)
    {
        /* Legacy clients (pre-version-handshake) send identity directly.
         * Log once so the connection is visible, then fall through with
         * negotiated_version = LEGACY (0). */
        if (!this->version_received)
        {
            FSS_LOG_WARN("server", "Legacy client: no protocol version handshake (assuming version 0)");
            this->version_received = true;
        }
        if (msg->getType() == fss::transport::message_type_identity)
        {
            auto identity_msg = std::dynamic_pointer_cast<fss::transport::fss_message_identity>(msg);
            if (identity_msg != nullptr)
            {
                auto client_name = identity_msg->getName();
                auto possible_names = active_conn->getClientNames();
                bool name_valid = false;
                if (possible_names.empty())
                {
                    FSS_LOG_ERROR("server", "Rejecting client: no CN found in certificate");
                }
                else
                {
                    name_valid = std::any_of(possible_names.begin(), possible_names.end(),
                                             [&client_name](const auto &n) -> auto { return n == client_name; });
                }
                if (!name_valid)
                {
                    if (!possible_names.empty())
                    {
                        /* The empty-CN case logged above; this is the
                         * cert-present-but-name-differs case — a
                         * misconfigured client at best, an impersonation
                         * attempt at worst. Name both sides so the
                         * mismatch is diagnosable from this line alone. */
                        FSS_LOG_ERROR("server", "Rejecting identity '" << client_name
                                                                       << "': does not match certificate CN '"
                                                                       << possible_names.front() << "'");
                    }
                    this->client_handler->clientDisconnected(this);
                    return;
                }
                auto asset_id_opt = this->dbc->getAssetId(client_name);
                if (!asset_id_opt.has_value())
                {
                    /* The lookup itself failed (DB read error) -- distinct
                     * from a genuinely unknown asset below (todo/60), so a
                     * read outage reads as a DB incident, not a fleet of bad
                     * CNs. A client whose identify lands here will redial and
                     * be rejected again on every reconnect tick until the
                     * outage clears. */
                    FSS_LOG_ERROR("server",
                                  "Rejecting identity '" << client_name << "': asset lookup failed (DB read error)");
                    this->client_handler->clientDisconnected(this);
                    return;
                }
                uint64_t asset_id = *asset_id_opt;
                if (asset_id == 0)
                {
                    /* The query ran fine and found no asset by this name --
                     * an unregistered/misconfigured client, not a DB
                     * problem. Recurs at reconnect-backoff cadence, which is
                     * the visibility we want for this case. */
                    FSS_LOG_WARN("server",
                                 "Rejecting identity '" << client_name << "': no matching asset in the database");
                    this->client_handler->clientDisconnected(this);
                    return;
                }
                /* todo/31: apply the configured duplicate-identity policy
                 * before this session is marked identified, so a rejected
                 * newcomer never gets far enough to receive commands or
                 * have its telemetry stored against the shared asset_id.
                 * The handler reserves asset_id for this client atomically
                 * with the check (todo/44); the store below publishes the
                 * id for the poller and telemetry paths. */
                if (!this->client_handler->resolveDuplicateIdentity(this, asset_id))
                {
                    FSS_LOG_WARN("server", "Rejecting duplicate identity for asset_id "
                                               << asset_id << " (" << client_name
                                               << "): an existing session is already live");
                    this->client_handler->clientDisconnected(this);
                    return;
                }
                this->cached_asset_id.store(asset_id);
                this->setPendingCommand(this->dbc->getCommand(asset_id));
                {
                    std::scoped_lock guard(this->client_lock);
                    this->name = std::move(client_name);
                    this->liveness_active = true;
                    this->last_rtt_response_time = this->clock->now_ms();
                }
                this->aircraft = true;
                this->identified = true;
                FSS_LOG_INFO("server", "Aircraft client identified: " << this->getName());
                this->sendCommand();
                /* Identify runs on the recv thread, where a synchronous DB
                 * read is allowed — prime the cache so the settings go out
                 * now rather than after the poller's next refresh. */
                this->refreshSmmSettings();
                this->sendSMMSettings();
                /* A failed read (nullptr) skips the send; the client gets
                 * the list from the poller's 15 s broadcast instead. */
                if (auto server_list_msg = getServersListMsg(this->dbc))
                {
                    active_conn->sendMsg(server_list_msg);
                }
                else
                {
                    FSS_LOG_WARN("server", "Server-list read failed during identify of "
                                               << this->getName() << "; relying on the periodic broadcast");
                }
            }
        }
        else if (msg->getType() == fss::transport::message_type_identity_non_aircraft)
        {
            auto possible_names = active_conn->getClientNames();
            if (possible_names.empty())
            {
                FSS_LOG_ERROR("server", "Rejecting non-aircraft client: no CN found in certificate");
                this->client_handler->clientDisconnected(this);
                return;
            }
            const auto &client_name = possible_names.front();
            auto asset_id_opt = this->dbc->getAssetId(client_name);
            if (!asset_id_opt.has_value())
            {
                /* Can't prove this CN isn't an aircraft's -- treat the read
                 * failure the same as the aircraft branch above (todo/60),
                 * not as "belongs to a known aircraft". */
                FSS_LOG_ERROR("server", "Rejecting non-aircraft client: asset lookup failed for CN "
                                            << client_name << " (DB read error)");
                this->client_handler->clientDisconnected(this);
                return;
            }
            if (*asset_id_opt != 0)
            {
                FSS_LOG_ERROR("server",
                              "Rejecting non-aircraft client: CN " << client_name << " belongs to a known aircraft");
                this->client_handler->clientDisconnected(this);
                return;
            }
            {
                std::scoped_lock guard(this->client_lock);
                this->name = client_name;
                /* Enable liveness tracking so a dead non-aircraft client is
                 * reaped on RTT timeout, the same as an aircraft. RTT requests
                 * already go to every client and responses update
                 * last_rtt_response_time regardless of type. */
                this->liveness_active = true;
                this->last_rtt_response_time = this->clock->now_ms();
            }
            this->aircraft = false;
            this->identified = true;
            FSS_LOG_INFO("server", "Non-aircraft client identified: " << client_name);
        }
        else
        {
            this->sendMsg(std::make_shared<fss::transport::fss_message_identity_required>());
        }
    }
    else
    {
        /* todo/37: rtt_response must never be rate-limited alongside bulk
         * telemetry — a dropped response makes a live, healthy connection
         * look timed out, since isTimedOut() only advances on a matched
         * response. It is exempted outright rather than given its own bucket:
         * an unmatched response only ever does bounded work (outstanding_rtt_
         * requests and stray_rtt_responses are both small and hard-capped),
         * so there is no flood vector to size a bucket against.
         *
         * command_ack gets its own small bucket instead of a blanket
         * exemption. A dropped ack is a permanent loss of the command/ack
         * audit link (the FMU sends one exactly once and never resends), but
         * unlike rtt_response an ack is *not* naturally bounded: nothing
         * in-session validates it against a command this server actually
         * dispatched before enqueueing, and command_ack_write is a protected
         * task in the shared db_write_queue that can evict other assets'
         * telemetry and even other command writes under sustained pressure
         * (todo/20). Fully exempting it would let one identified peer flood
         * that queue at line rate. The dedicated bucket is sized well above
         * any legitimate cadence (a command produces at most two acks) while
         * still capping a flood far below "unbounded". */
        bool exempt_from_rate_limit = msg->getType() == fss::transport::message_type_rtt_response;
        bool is_command_ack = msg->getType() == fss::transport::message_type_command_ack;
        bool rate_ok = exempt_from_rate_limit || (is_command_ack ? this->command_ack_rate.consume(this->clock->now_ms())
                                                                 : this->msg_rate.consume(this->clock->now_ms()));
        if (!rate_ok)
        {
            auto now_ms = this->clock->now_ms();
            if (is_command_ack)
            {
                /* Distinct counters/log from telemetry drops (todo/37 item
                 * 3): a lost ack must never look like an ordinary telemetry
                 * drop in the logs. */
                ++this->ack_rate_limit_rejects;
                FSS_LOG_DEBUG("server", "Rate-limiting command_ack from "
                                            << this->name << " (rejects=" << this->ack_rate_limit_rejects << ")");
                if (this->last_ack_rate_limit_log_ms == 0)
                {
                    this->last_ack_rate_limit_log_ms = now_ms;
                }
                else if (now_ms - this->last_ack_rate_limit_log_ms >= 1000)
                {
                    FSS_LOG_WARN("server", "Rate-limiting command_ack from "
                                               << this->name << " - dropped " << this->ack_rate_limit_rejects
                                               << " acks in the last " << (now_ms - this->last_ack_rate_limit_log_ms)
                                               << "ms");
                    this->ack_rate_limit_rejects = 0;
                    this->last_ack_rate_limit_log_ms = now_ms;
                }
                return;
            }
            ++this->rate_limit_rejects;
            FSS_LOG_DEBUG("server",
                          "Rate-limiting client " << this->name << " (rejects=" << this->rate_limit_rejects << ")");
            if (this->last_rate_limit_log_ms == 0)
            {
                this->last_rate_limit_log_ms = now_ms;
            }
            else if (now_ms - this->last_rate_limit_log_ms >= 1000)
            {
                FSS_LOG_WARN("server", "Rate-limiting client " << this->name << " - dropped "
                                                               << this->rate_limit_rejects << " messages in the last "
                                                               << (now_ms - this->last_rate_limit_log_ms) << "ms");
                this->rate_limit_rejects = 0;
                this->last_rate_limit_log_ms = now_ms;
            }
            return;
        }
        uint64_t asset_id = this->cached_asset_id.load();
        switch (msg->getType())
        {
            case fss::transport::message_type_unknown:
            case fss::transport::message_type_closed:
            case fss::transport::message_type_identity:
            case fss::transport::message_type_identity_non_aircraft:
            case fss::transport::message_type_identity_required:
            case fss::transport::message_type_version: break;
            case fss::transport::message_type_rtt_request: {
                auto reply_msg = std::make_shared<fss::transport::fss_message_rtt_response>(msg->getId());
                active_conn->sendMsg(reply_msg);
            }
            break;
            case fss::transport::message_type_rtt_response: {
                std::shared_ptr<fss_client_rtt> rtt_req = nullptr;
                uint64_t current_ts = this->clock->now_ms();
                /* Captured adjacent to the monotonic receive time above so the
                 * wall and monotonic readings refer to the same instant (see
                 * updateClockOffset). */
                uint64_t recv_wall = fss::fss_current_timestamp();
                auto rtt_resp_msg = std::dynamic_pointer_cast<fss::transport::fss_message_rtt_response>(msg);
                if (rtt_resp_msg != nullptr)
                {
                    std::scoped_lock guard(this->client_lock);
                    for (const auto &req : this->outstanding_rtt_requests)
                    {
                        if (req->getRequestId() == rtt_resp_msg->getRequestId())
                        {
                            rtt_req = req;
                        }
                    }
                    if (rtt_req != nullptr)
                    {
                        this->outstanding_rtt_requests.remove(rtt_req);
                        this->last_rtt_response_time = this->clock->now_ms();
                    }
                    else
                    {
                        /* todo/35: sendRTTRequest() cannot push its bookkeeping
                         * entry until sendMsg() returns (the id is only stamped
                         * onto the message inside that call), so an
                         * exceptionally fast response can arrive here first and
                         * find no match. Remember it, bounded, so
                         * sendRTTRequest() can reconcile instead of the request
                         * being reaped as timed out despite an already-answered
                         * peer. */
                        if (this->stray_rtt_responses.size() >= max_stray_rtt_responses)
                        {
                            this->stray_rtt_responses.pop_front();
                        }
                        this->stray_rtt_responses.push_back(
                            std::make_shared<fss_client_rtt>(current_ts, rtt_resp_msg->getRequestId()));
                    }
                }
                if (rtt_req != nullptr)
                {
                    uint64_t rtt_ms = current_ts - rtt_req->getTimeStamp();
                    if (asset_id != 0)
                    {
#ifdef DEBUG
                        std::cout << "RTT for " << this->getName() << " is " << rtt_ms << std::endl;
#endif
                        this->writer->enqueue(rtt_write{asset_id, rtt_ms});
                    }
                    /* RTT clock-offset (todo/17 item 3): only when the peer
                     * negotiated the capability — otherwise any trailing
                     * timestamp is not part of the agreed dialect and is ignored. */
                    if ((active_conn->getNegotiatedFeatureFlags() & fss::transport::FSS_FEATURE_RTT_OFFSET) != 0)
                    {
                        this->updateClockOffset(rtt_resp_msg->getClientTimestamp(), rtt_ms, recv_wall);
                    }
                }
            }
            break;
            case fss::transport::message_type_position_report: {
                uint64_t report_ts = msg->getTimeStamp();
                if (this->position_staleness_ms != 0 && report_ts > 0)
                {
                    uint64_t now = fss::fss_current_timestamp();
                    /* Compare the client-stamped time against the server clock,
                     * offset-corrected (client_clock_offset_ms is measured from
                     * RTT responses that carry the client's clock, else 0). The
                     * window is symmetric: a clock ahead of the server is as
                     * wrong as one behind, so future-dated reports are rejected
                     * too.
                     *
                     * Take the signed difference from the unsigned subtraction so
                     * the two epoch-ms values are never narrowed to int64 (the
                     * difference itself is small and always fits). */
                    int64_t diff = now >= report_ts ? static_cast<int64_t>(now - report_ts)
                                                    : -static_cast<int64_t>(report_ts - now);
                    int64_t skew = diff + this->client_clock_offset_ms;
                    int64_t magnitude = skew < 0 ? -skew : skew;
                    if (magnitude > static_cast<int64_t>(this->position_staleness_ms))
                    {
                        uint64_t discards = ++this->staleness_discards;
                        if (discards == 1)
                        {
                            FSS_LOG_WARN("server", "Stale position report from " << this->name << " (skew=" << skew
                                                                                 << "ms), discarding");
                        }
                        else if (discards == staleness_escalation_threshold ||
                                 discards % staleness_escalation_interval == 0)
                        {
                            FSS_LOG_ERROR("server", "Client " << this->name << " clock skew suspected (skew=" << skew
                                                              << "ms, " << discards
                                                              << " consecutive), discarding all positions");
                        }
                        return;
                    }
                    /* In-window: the clock is fine, so clear the skew streak
                     * even if the coordinate below is later rejected. */
                    this->staleness_discards = 0;
                }
                if (std::isnan(msg->getLatitude()) || std::isnan(msg->getLongitude()))
                {
                    /* NaN coordinates are the wire sentinel for "no GPS fix"
                     * (see pack_scaled_coord). This is operationally distinct
                     * from a malformed coordinate, so log it as such — but a
                     * persistent no-fix arrives every report, so throttle to
                     * first + every 100th to avoid flooding the log. */
                    uint64_t no_fix = ++this->no_fix_reports;
                    if (no_fix == 1 || (no_fix % 100) == 0)
                    {
                        FSS_LOG_WARN("server", "Position report with no GPS fix from "
                                                   << this->name << " (count=" << no_fix << "), discarding");
                    }
                    return;
                }
                if (!is_valid_coordinate(msg->getLatitude(), msg->getLongitude()))
                {
                    FSS_LOG_WARN("server", "Invalid position report coordinates (lat=" << msg->getLatitude()
                                                                                       << " lon=" << msg->getLongitude()
                                                                                       << "), discarding");
                    return;
                }
                if (this->aircraft && asset_id != 0)
                {
                    this->writer->enqueue(
                        position_write{asset_id, msg->getLatitude(), msg->getLongitude(), msg->getAltitude()});
                }
                this->client_handler->broadcastMsg(msg, this);
            }
            break;
            case fss::transport::message_type_system_status: {
                auto status_msg = std::dynamic_pointer_cast<fss::transport::fss_message_system_status>(msg);
                if (status_msg != nullptr && asset_id != 0)
                {
                    this->writer->enqueue(status_write{asset_id, status_msg->getBatRemaining(),
                                                       status_msg->getBatMAHUsed(), status_msg->getBatVoltage()});
                }
            }
            break;
            case fss::transport::message_type_search_status: {
                auto status_msg = std::dynamic_pointer_cast<fss::transport::fss_message_search_status>(msg);
                if (status_msg != nullptr && asset_id != 0)
                {
                    this->writer->enqueue(search_status_write{asset_id, status_msg->getSearchId(),
                                                              status_msg->getSearchCompleted(),
                                                              status_msg->getSearchTotal()});
                }
            }
            break;
            case fss::transport::message_type_command_ack: {
                /* An aircraft client acking a command we sent (todo/17 item 1).
                 * Only honour it when the peer negotiated the capability — a
                 * conforming client never sends one otherwise, so an ack without
                 * the flag is from a misbehaving/forged peer and is dropped. */
                if ((active_conn->getNegotiatedFeatureFlags() & fss::transport::FSS_FEATURE_COMMAND_ACK) == 0)
                {
                    break;
                }
                auto ack_msg = std::dynamic_pointer_cast<fss::transport::fss_message_command_ack>(msg);
                /* Scope the stored ack to the acking asset: dispatch_id is only
                 * per-connection unique, so without asset_id the DB update could
                 * land on a different asset's same-dispatch_id command row. An
                 * ack from a connection with no identified asset (asset_id == 0)
                 * cannot be scoped, so it is dropped. */
                if (ack_msg != nullptr && asset_id != 0)
                {
                    this->writer->enqueue(command_ack_write{
                        asset_id, ack_msg->getAckedCommandId(), static_cast<uint8_t>(ack_msg->getOutcome()),
                        ack_msg->getTimeStamp(), static_cast<uint8_t>(ack_msg->getReason())});
                }
                else if (ack_msg != nullptr)
                {
                    /* asset_id == 0: the connection has no identified asset, so the
                     * ack cannot be scoped and is dropped. A conforming client only
                     * acks after identifying, so this is unexpected — log it so an
                     * unscoped ack can be investigated rather than vanishing. */
                    FSS_LOG_WARN("server", "Dropping command-ack for acked-command "
                                               << ack_msg->getAckedCommandId()
                                               << " from connection with no identified asset (asset_id==0)");
                }
            }
            break;
            case fss::transport::message_type_command:
            case fss::transport::message_type_server_list:
            case fss::transport::message_type_smm_settings: break;
        }
    }
}

namespace flight_safety_system::server {
/* Exposed so server.cpp can broadcast the server list without duplicating
 * the helper. Not in the public header — only the server binary uses it.
 * nullptr when the DB read failed: the poller must keep the previous cached
 * list rather than replacing it (todo/24). */
auto build_server_list_msg(IDatabase *dbc) -> std::shared_ptr<transport::fss_message_server_list>
{
    return getServersListMsg(dbc);
}
} // namespace flight_safety_system::server
