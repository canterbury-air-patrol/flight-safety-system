#include "fss-transport.hpp"
#include "fss.hpp"
#include "fss-log.hpp"
#include "fss-server.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>

namespace fss = flight_safety_system;

static auto is_valid_coordinate(double latitude, double longitude) -> bool
{
    return std::isfinite(latitude) && std::isfinite(longitude) && latitude >= -90.0 && latitude <= 90.0 &&
           longitude >= -180.0 && longitude <= 180.0;
}

constexpr uint64_t sec_to_msec = 1000;
constexpr uint64_t rtt_retry_interval = 10 * sec_to_msec;

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
    this->getConnection()->setHandler(this);
}

fss::server::fss_client::~fss_client() = default;

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
    std::scoped_lock guard(this->client_lock);
    if (this->cached_asset_id.load() == 0)
    {
        return;
    }
    uint64_t ts = this->clock->now_ms();
    auto ac = this->pending_command;
    constexpr int timeout_time = 10 * sec_to_msec;
    if (ac != nullptr && (ac->getDBId() != this->last_command_dbid || ts > (this->last_command_send_ts + timeout_time)))
    {
        this->last_command_send_ts = ts;
        this->last_command_dbid = ac->getDBId();
        auto command = ac->getCommand();
        if (command == fss::transport::asset_command_unknown)
        {
            FSS_LOG_ERROR("server", "Refusing to dispatch unknown command type to "
                                        << this->name << " (dbid=" << ac->getDBId() << ")");
            return;
        }
        if (command == fss::transport::asset_command_goto &&
            !is_valid_coordinate(ac->getLatitude(), ac->getLongitude()))
        {
            FSS_LOG_ERROR("server", "Refusing to dispatch GOTO command with invalid coordinates to "
                                        << this->name << " (dbid=" << ac->getDBId() << ", lat=" << ac->getLatitude()
                                        << ", lon=" << ac->getLongitude() << ")");
            return;
        }
        if (command == fss::transport::asset_command_altitude && !ac->isAltitudeValid())
        {
            /* A NULL altitude must not dispatch as 0 — that is a
             * descend-to-ground instruction. */
            FSS_LOG_ERROR("server", "Refusing to dispatch ALT command with NULL altitude to "
                                        << this->name << " (dbid=" << ac->getDBId() << ")");
            return;
        }
        std::shared_ptr<fss::transport::fss_message_asset_command> msg = nullptr;
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
        this->getConnection()->sendMsg(msg);
        FSS_LOG_INFO("server", "dispatched command dbid=" << ac->getDBId() << " to " << this->name);
    }
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
    bool timed_out = false;
    {
        std::scoped_lock guard(this->client_lock);
        uint64_t now = this->clock->now_ms();
        if (!this->outstanding_rtt_requests.empty())
        {
            if (now - this->outstanding_rtt_requests.front()->getTimeStamp() > this->client_timeout_ms)
            {
                timed_out = true;
            }
            else if (now - this->outstanding_rtt_requests.back()->getTimeStamp() < rtt_retry_interval)
            {
                return;
            }
        }
        if (!timed_out)
        {
            this->getConnection()->sendMsg(rtt_req);
            this->outstanding_rtt_requests.push_back(std::make_shared<fss_client_rtt>(now, rtt_req->getId()));
        }
    }
    if (timed_out)
    {
        this->client_handler->clientDisconnected(this);
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
        this->getConnection()->sendMsg(settings_msg);
    }
}

namespace {
auto getServersListMsg(fss::server::IDatabase *dbc) -> std::shared_ptr<fss::transport::fss_message_server_list>
{
    auto server_list = std::make_shared<fss::transport::fss_message_server_list>();
    for (auto &server_details : dbc->getActiveServers())
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
            this->getConnection()->setNegotiatedVersion(negotiated);
            this->version_received = true;
            if (negotiated >= 2)
            {
                this->expected_seq.store(msg->getId() + 1);
            }
            auto resp = std::make_shared<fss::transport::fss_message_version>();
            this->getConnection()->sendMsg(resp);
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
    if (this->getConnection()->getNegotiatedVersion() >= 2)
    {
        uint64_t wanted = this->expected_seq.load();
        if (wanted != 0)
        {
            if (msg->getSeq() != wanted)
            {
                FSS_LOG_WARN("server",
                             "Out-of-order or duplicate message seq=" << msg->getSeq() << " expected=" << wanted);
                if (msg->getType() == fss::transport::message_type_identity ||
                    msg->getType() == fss::transport::message_type_identity_non_aircraft)
                {
                    this->client_handler->clientDisconnected(this);
                    return;
                }
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
                auto possible_names = this->getConnection()->getClientNames();
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
                    this->client_handler->clientDisconnected(this);
                    return;
                }
                uint64_t asset_id = this->dbc->getAssetId(client_name);
                if (asset_id == 0)
                {
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
                this->getConnection()->sendMsg(getServersListMsg(this->dbc));
            }
        }
        else if (msg->getType() == fss::transport::message_type_identity_non_aircraft)
        {
            auto possible_names = this->getConnection()->getClientNames();
            if (possible_names.empty())
            {
                FSS_LOG_ERROR("server", "Rejecting non-aircraft client: no CN found in certificate");
                this->client_handler->clientDisconnected(this);
                return;
            }
            const auto &client_name = possible_names.front();
            if (this->dbc->getAssetId(client_name) != 0)
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
        if (!this->msg_rate.consume(this->clock->now_ms()))
        {
            auto now_ms = this->clock->now_ms();
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
                this->getConnection()->sendMsg(reply_msg);
            }
            break;
            case fss::transport::message_type_rtt_response: {
                std::shared_ptr<fss_client_rtt> rtt_req = nullptr;
                uint64_t current_ts = this->clock->now_ms();
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
                }
                if (rtt_req != nullptr && asset_id != 0)
                {
#ifdef DEBUG
                    std::cout << "RTT for " << this->getName() << " is " << (current_ts - rtt_req->getTimeStamp())
                              << std::endl;
#endif
                    this->writer->enqueue(rtt_write{asset_id, current_ts - rtt_req->getTimeStamp()});
                }
            }
            break;
            case fss::transport::message_type_position_report: {
                constexpr uint64_t staleness_limit_ms = 30000;
                uint64_t report_ts = msg->getTimeStamp();
                if (report_ts > 0)
                {
                    uint64_t now = fss::fss_current_timestamp();
                    if (now > report_ts + staleness_limit_ms)
                    {
                        FSS_LOG_WARN("server", "Stale position report (age=" << (now - report_ts) << "ms), discarding");
                        return;
                    }
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
            case fss::transport::message_type_command:
            case fss::transport::message_type_server_list:
            case fss::transport::message_type_smm_settings: break;
        }
    }
}

namespace flight_safety_system::server {
/* Exposed so server.cpp can broadcast the server list without duplicating
 * the helper. Not in the public header — only the server binary uses it. */
auto build_server_list_msg(IDatabase *dbc) -> std::shared_ptr<transport::fss_message_server_list>
{
    return getServersListMsg(dbc);
}
} // namespace flight_safety_system::server
