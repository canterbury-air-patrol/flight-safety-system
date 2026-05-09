#include "fss-transport.hpp"
#include "fss.hpp"
#include "fss-log.hpp"
#include "fss-server.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>

namespace fss = flight_safety_system;

constexpr uint64_t sec_to_msec = 1000;
constexpr uint64_t rtt_retry_interval = 10 * sec_to_msec;

fss::server::smm_settings::smm_settings(std::string t_address, fss::secure_string t_username, fss::secure_string t_password) : address(std::move(t_address)), username(std::move(t_username)), password(std::move(t_password))
{
}

auto fss::server::smm_settings::getAddress() -> std::string { return this->address; }
auto fss::server::smm_settings::getUsername() -> const fss::secure_string & { return this->username; }
auto fss::server::smm_settings::getPassword() -> const fss::secure_string & { return this->password; }

fss::server::fss_server_details::fss_server_details(std::string t_address, uint16_t t_port) : address(std::move(t_address)), port(t_port)
{
}

auto fss::server::fss_server_details::getAddress() -> std::string { return this->address; }
auto fss::server::fss_server_details::getPort() -> uint16_t { return this->port; }

fss::server::asset_command::asset_command(uint64_t t_dbid, uint64_t t_timestamp, const std::string &t_cmd, double t_latitude, double t_longitude, uint16_t t_altitude) : dbid(t_dbid), timestamp(t_timestamp), latitude(t_latitude), longitude(t_longitude), altitude(t_altitude)
{
    if (t_cmd == "RTL") {
        this->command = transport::asset_command_rtl;
    } else if (t_cmd == "HOLD") {
        this->command = transport::asset_command_hold;
    } else if (t_cmd == "GOTO") {
        this->command = transport::asset_command_goto;
    } else if (t_cmd == "RON") {
        this->command = transport::asset_command_resume;
    } else if (t_cmd == "DISARM") {
        this->command = transport::asset_command_disarm;
    } else if (t_cmd == "ALT") {
        this->command = transport::asset_command_altitude;
    } else if (t_cmd == "TERM") {
        this->command = transport::asset_command_terminate;
    } else if (t_cmd == "MAN") {
        this->command = transport::asset_command_manual;
    }
}

auto fss::server::asset_command::getDBId() -> uint64_t { return this->dbid; }
auto fss::server::asset_command::getTimeStamp() -> uint64_t { return this->timestamp; }
auto fss::server::asset_command::getCommand() -> fss::transport::fss_asset_command { return this->command; }
auto fss::server::asset_command::getLatitude() -> double { return this->latitude; }
auto fss::server::asset_command::getLongitude() -> double { return this->longitude; }
auto fss::server::asset_command::getAltitude() -> uint16_t { return this->altitude; }

fss::server::fss_client_rtt::fss_client_rtt(uint64_t t_timestamp, uint64_t t_reqid) : timestamp(t_timestamp), reqid(t_reqid)
{
}

auto fss::server::fss_client_rtt::getTimeStamp() -> uint64_t { return this->timestamp; }
auto fss::server::fss_client_rtt::getRequestId() -> uint64_t { return this->reqid; }

fss::server::fss_client::fss_client(std::shared_ptr<fss::transport::fss_connection> t_conn, IDatabase *t_dbc, std::shared_ptr<db_write_queue> t_writer, fss_client_handler *t_handler) : fss_message_cb(std::move(t_conn)), dbc(t_dbc), writer(std::move(t_writer)), client_handler(t_handler)
{
    this->getConnection()->setHandler(this);
}

fss::server::fss_client::~fss_client() = default;

auto
fss::server::fss_client::isAircraft() -> bool
{
    return this->aircraft;
}

auto
fss::server::fss_client::getName() -> std::string
{
    std::lock_guard<std::mutex> guard(this->client_lock);
    return this->name;
}

void
fss::server::fss_client::sendCommand()
{
    std::lock_guard<std::mutex> guard(this->client_lock);
    uint64_t ts = fss_current_timestamp();
    uint64_t asset_id = this->dbc->getAssetId(this->name);
    if (asset_id == 0) { return; }
    auto ac = this->dbc->getCommand(asset_id);
    constexpr int timeout_time = 10 * sec_to_msec;
    if (ac != nullptr && (ac->getDBId() != this->last_command_dbid || ts > (this->last_command_send_ts + timeout_time)))
    {
        this->last_command_send_ts = ts;
        this->last_command_dbid = ac->getDBId();
        std::shared_ptr<fss::transport::fss_message_asset_command> msg = nullptr;
        auto command = ac->getCommand();
        switch (command)
        {
            case fss::transport::asset_command_goto:
                msg = std::make_shared<fss::transport::fss_message_asset_command>(command, ac->getTimeStamp(), ac->getLatitude(), ac->getLongitude());
                break;
            case fss::transport::asset_command_altitude:
                msg = std::make_shared<fss::transport::fss_message_asset_command>(command, ac->getTimeStamp(), ac->getAltitude());
                break;
            default:
                msg = std::make_shared<fss::transport::fss_message_asset_command>(command, ac->getTimeStamp());
                break;
        }
        this->getConnection()->sendMsg(msg);
        FSS_LOG_INFO("server", "dispatched command dbid=" << ac->getDBId() << " to " << this->name);
    }
}

void
fss::server::fss_client::setClock(std::shared_ptr<fss::IClock> t_clock)
{
    if (t_clock == nullptr)
    {
        return;
    }
    this->clock = std::move(t_clock);
}

void
fss::server::fss_client::setTimeoutMs(uint64_t ms)
{
    std::lock_guard<std::mutex> guard(this->client_lock);
    this->client_timeout_ms = ms;
}

void
fss::server::fss_client::setRateLimits(uint64_t capacity, uint64_t refill_per_s)
{
    this->msg_rate = rate_limiter(capacity, refill_per_s);
}

auto
fss::server::fss_client::isTimedOut() -> bool
{
    std::lock_guard<std::mutex> guard(this->client_lock);
    if (!this->liveness_active) { return false; }
    return (this->clock->now_ms() - this->last_rtt_response_time) > this->client_timeout_ms;
}

void
fss::server::fss_client::sendRTTRequest(const std::shared_ptr<fss::transport::fss_message_rtt_request> &rtt_req)
{
    bool timed_out = false;
    {
        std::lock_guard<std::mutex> guard(this->client_lock);
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

void
fss::server::fss_client::sendSMMSettings()
{
    std::string client_name;
    {
        std::lock_guard<std::mutex> guard(this->client_lock);
        client_name = this->name;
    }
    uint64_t asset_id = this->dbc->getAssetId(client_name);
    if (asset_id == 0) { return; }
    auto smm = this->dbc->getSmmSettings(asset_id);
    if (smm != nullptr)
    {
        auto settings_msg = std::make_shared<fss::transport::fss_message_smm_settings>(smm->getAddress(), smm->getUsername(), smm->getPassword());
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

void
fss::server::fss_client::processMessage(std::shared_ptr<fss::transport::fss_message> msg)
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
        this->client_handler->clientDisconnected(this);
        return;
    }
    if (msg->getType() == fss::transport::message_type_version)
    {
        auto version_msg = std::dynamic_pointer_cast<fss::transport::fss_message_version>(msg);
        if (version_msg != nullptr)
        {
            uint16_t peer_version = version_msg->getProtocolVersion();
            uint16_t peer_min = version_msg->getMinSupportedVersion();
            if (peer_version < fss::transport::FSS_PROTOCOL_MIN_VERSION
                || peer_min > fss::transport::FSS_PROTOCOL_VERSION)
            {
                FSS_LOG_ERROR("server", "Client protocol version incompatible: peer=" << peer_version << " peer_min=" << peer_min << " us=" << fss::transport::FSS_PROTOCOL_VERSION << " us_min=" << fss::transport::FSS_PROTOCOL_MIN_VERSION);
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
    if (this->getConnection()->getNegotiatedVersion() >= 2)
    {
        uint64_t wanted = this->expected_seq.load();
        if (wanted != 0)
        {
            if (msg->getSeq() != wanted)
            {
                FSS_LOG_WARN("server", "Out-of-order or replayed message seq=" << msg->getSeq() << " expected=" << wanted);
                if (msg->getType() == fss::transport::message_type_identity
                    || msg->getType() == fss::transport::message_type_identity_non_aircraft)
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
        /* todo02: legacy clients (pre-version-handshake) send identity
         * directly. Log once so the legacy connection is visible, then
         * fall through with negotiated_version = LEGACY (0). */
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
                    name_valid = std::any_of(possible_names.begin(), possible_names.end(), [&client_name](const auto &n) -> auto {
                        return n == client_name;
                    });
                }
                if (!name_valid)
                {
                    this->client_handler->clientDisconnected(this);
                    return;
                }
                if (this->dbc->getAssetId(client_name) == 0)
                {
                    this->client_handler->clientDisconnected(this);
                    return;
                }
                {
                    std::lock_guard<std::mutex> guard(this->client_lock);
                    this->name = std::move(client_name);
                    this->liveness_active = true;
                    this->last_rtt_response_time = this->clock->now_ms();
                }
                this->aircraft = true;
                this->identified = true;
                this->sendCommand();
                this->sendSMMSettings();
                this->getConnection()->sendMsg(getServersListMsg(this->dbc));
            }
        }
        else if(msg->getType() == fss::transport::message_type_identity_non_aircraft)
        {
            /* todo16: identify the non-aircraft client by its leaf cert CN.
             * Without this check any holder of any cert valid against the
             * CA could become a non-aircraft session anonymously, bypassing
             * the per-asset identity contract aircraft connections enforce
             * (todo15). */
            auto possible_names = this->getConnection()->getClientNames();
            if (possible_names.empty())
            {
                FSS_LOG_ERROR("server", "Rejecting non-aircraft client: no CN found in certificate");
                this->client_handler->clientDisconnected(this);
                return;
            }
            {
                std::lock_guard<std::mutex> guard(this->client_lock);
                this->name = possible_names.front();
            }
            this->aircraft = false;
            this->identified = true;
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
            FSS_LOG_DEBUG("server", "Rate-limiting client " << this->name
                                                            << " (rejects=" << this->rate_limit_rejects << ")");
            if (this->last_rate_limit_log_ms == 0)
            {
                this->last_rate_limit_log_ms = now_ms;
            }
            else if (now_ms - this->last_rate_limit_log_ms >= 1000)
            {
                FSS_LOG_WARN("server", "Rate-limiting client " << this->name
                                                               << " - dropped " << this->rate_limit_rejects
                                                               << " messages in the last "
                                                               << (now_ms - this->last_rate_limit_log_ms) << "ms");
                this->rate_limit_rejects = 0;
                this->last_rate_limit_log_ms = now_ms;
            }
            return;
        }
        std::string client_name = this->getName();
        uint64_t asset_id = this->dbc->getAssetId(client_name);
        switch (msg->getType())
        {
            case fss::transport::message_type_unknown:
            case fss::transport::message_type_closed:
            case fss::transport::message_type_identity:
            case fss::transport::message_type_identity_non_aircraft:
            case fss::transport::message_type_identity_required:
            case fss::transport::message_type_version:
                break;
            case fss::transport::message_type_rtt_request:
            {
                auto reply_msg = std::make_shared<fss::transport::fss_message_rtt_response>(msg->getId());
                this->getConnection()->sendMsg(reply_msg);
            }
                break;
            case fss::transport::message_type_rtt_response:
            {
                std::shared_ptr<fss_client_rtt> rtt_req = nullptr;
                uint64_t current_ts = fss_current_timestamp();
                auto rtt_resp_msg = std::dynamic_pointer_cast<fss::transport::fss_message_rtt_response>(msg);
                if (rtt_resp_msg != nullptr)
                {
                    std::lock_guard<std::mutex> guard(this->client_lock);
                    for(const auto &req : this->outstanding_rtt_requests)
                    {
                        if(req->getRequestId() == rtt_resp_msg->getRequestId())
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
                    std::cout << "RTT for " << client_name << " is " << (current_ts - rtt_req->getTimeStamp()) << std::endl;
#endif
                    this->writer->enqueue(rtt_write{asset_id, current_ts - rtt_req->getTimeStamp()});
                }
            }
                break;
            case fss::transport::message_type_position_report:
            {
                constexpr uint64_t staleness_limit_ms = 30000;
                uint64_t report_ts = msg->getTimeStamp();
                if (report_ts > 0)
                {
                    uint64_t now = this->clock->now_ms();
                    if (now > report_ts + staleness_limit_ms)
                    {
                        FSS_LOG_WARN("server", "Stale position report (age=" << (now - report_ts) << "ms), discarding");
                        return;
                    }
                }
                if (this->aircraft && asset_id != 0)
                {
                    this->writer->enqueue(position_write{asset_id, msg->getLatitude(), msg->getLongitude(), msg->getAltitude()});
                }
                this->client_handler->broadcastMsg(msg, this);
            }
                break;
            case fss::transport::message_type_system_status:
            {
                auto status_msg = std::dynamic_pointer_cast<fss::transport::fss_message_system_status>(msg);
                if (status_msg != nullptr && asset_id != 0)
                {
                    this->writer->enqueue(status_write{asset_id, status_msg->getBatRemaining(), status_msg->getBatMAHUsed(), status_msg->getBatVoltage()});
                }
            }
                break;
            case fss::transport::message_type_search_status:
            {
                auto status_msg = std::dynamic_pointer_cast<fss::transport::fss_message_search_status>(msg);
                if (status_msg != nullptr && asset_id != 0)
                {
                    this->writer->enqueue(search_status_write{asset_id, status_msg->getSearchId(), status_msg->getSearchCompleted(), status_msg->getSearchTotal()});
                }
            }
                break;
            case fss::transport::message_type_command:
            case fss::transport::message_type_server_list:
            case fss::transport::message_type_smm_settings:
                break;
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
