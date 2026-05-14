#include "fss-server.hpp"
#include "fss.hpp"
#include "fss-log.hpp"

extern "C" {
#include "server-db.h"
#include <string.h>
}

#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

flight_safety_system::server::db_connection::db_connection(std::string host, int port, std::string user, std::string pass, std::string db) // NOLINT(bugprone-easily-swappable-parameters)
    : db_lock(), host_(std::move(host)), port_(port), user_(std::move(user)), pass_(std::move(pass)), db_(std::move(db))
{
    connected_ = (db_connect(host_.c_str(), port_, user_.c_str(), pass_.c_str(), db_.c_str()) == 1);
}

auto
flight_safety_system::server::db_connection::isConnected() const -> bool
{
    return connected_;
}

void
flight_safety_system::server::db_connection::tryReconnectIfNeeded()
{
    std::scoped_lock guard(this->db_lock);
    if (db_ping() != 0)
    {
        connected_ = true;
        return;
    }
    FSS_LOG_WARN("db", "Database connection lost, attempting reconnect");
    db_disconnect();
    connected_ = (db_connect(host_.c_str(), port_, user_.c_str(), pass_.c_str(), db_.c_str()) == 1);
    if (connected_)
    {
        FSS_LOG_INFO("db", "Database reconnected successfully");
    }
    else
    {
        FSS_LOG_ERROR("db", "Database reconnect failed");
    }
}

flight_safety_system::server::db_connection::~db_connection()
{
    db_disconnect();
}

auto
flight_safety_system::server::db_connection::getAssetId(const std::string &name) -> uint64_t
{
    std::scoped_lock guard(this->db_lock);
    return db_get_asset_id(name.c_str());
}

void
flight_safety_system::server::db_connection::recordRtt(uint64_t asset_id, uint64_t rtt_ms)
{
    std::scoped_lock guard(this->db_lock);
    db_rtt_create_entry(asset_id, rtt_ms);
}

void
flight_safety_system::server::db_connection::recordStatus(uint64_t asset_id, uint8_t bat_percent, uint32_t bat_mah_used, double bat_voltage)
{
    std::scoped_lock guard(this->db_lock);
    db_status_create_entry(asset_id, bat_percent, bat_mah_used, bat_voltage);
}

void
flight_safety_system::server::db_connection::recordSearchStatus(uint64_t asset_id, uint64_t search_id, uint64_t completed, uint64_t total)
{
    std::scoped_lock guard(this->db_lock);
    db_search_status_create_entry(asset_id, search_id, completed, total);
}

void
flight_safety_system::server::db_connection::recordPosition(uint64_t asset_id, double latitude, double longitude, uint32_t altitude)
{
    std::scoped_lock guard(this->db_lock);
    if (altitude > static_cast<uint32_t>(std::numeric_limits<int>::max()))
    {
        FSS_LOG_ERROR("db", "Altitude " << altitude << " exceeds int range; discarding position record for asset " << asset_id);
        return;
    }
    db_position_create_entry(asset_id, latitude, longitude, static_cast<int>(altitude));
}

auto
flight_safety_system::server::db_connection::getCommand(uint64_t asset_id) -> std::shared_ptr<asset_command>
{
    std::shared_ptr<asset_command> res = nullptr;
    {
        std::scoped_lock guard(this->db_lock);
        struct asset_command_s *command = db_asset_command_get(asset_id);
        if (command)
        {
            res = std::make_shared<asset_command>(command->dbid, command->timestamp, std::string(command->command), command->latitude, command->longitude, command->altitude);
            free (command->command);
            free (command);
        }
    }
    return res;
}

auto
flight_safety_system::server::db_connection::getSmmSettings(uint64_t asset_id) -> std::shared_ptr<smm_settings>
{
    std::shared_ptr<smm_settings> res = nullptr;
    {
        std::scoped_lock guard(this->db_lock);
        struct smm_settings_s *settings = db_asset_smm_settings_get(asset_id);
        if (settings)
        {
            res = std::make_shared<smm_settings>(std::string(settings->address), flight_safety_system::secure_string(std::string_view(settings->username)), flight_safety_system::secure_string(std::string_view(settings->password)));
            free(settings->address);
            explicit_bzero(settings->username, strlen(settings->username));
            free(settings->username);
            explicit_bzero(settings->password, strlen(settings->password));
            free(settings->password);
            free(settings);
        }
    }
    return res;
}

auto
flight_safety_system::server::db_connection::getActiveServers() -> std::vector<fss_server_details>
{
    std::vector<fss_server_details> res;
    struct fss_server_s **servers = nullptr;
    {
        std::scoped_lock guard(this->db_lock);
        servers = db_active_fss_servers_get();
    }
    if (servers)
    {
        for(size_t i = 0; servers[i] != nullptr; i++)
        {
            res.emplace_back(servers[i]->address, servers[i]->port);
            free (servers[i]->address);
            free (servers[i]);
        }
        db_free_fss_servers(servers);
    }
    return res;
}
