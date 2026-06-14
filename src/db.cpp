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

flight_safety_system::server::db_connection::db_connection(
    std::string host, int port, std::string user, std::string pass,
    std::string db) // NOLINT(bugprone-easily-swappable-parameters)
    : read_lock(), write_lock(), host_(std::move(host)), port_(port), user_(std::move(user)), pass_(std::move(pass)),
      db_(std::move(db))
{
    read_connected_ = connectOne(read_conn_name);
    write_connected_ = connectOne(write_conn_name);
}

auto flight_safety_system::server::db_connection::connectOne(const char *conn_name) -> bool
{
    return db_connect(conn_name, host_.c_str(), port_, user_.c_str(), pass_.c_str(), db_.c_str()) == 1;
}

auto flight_safety_system::server::db_connection::isConnected() const -> bool
{
    return read_connected_.load() && write_connected_.load();
}

void flight_safety_system::server::db_connection::reconnectOne(const char *conn_name, std::atomic<bool> &connected_flag)
{
    if (db_ping(conn_name) != 0)
    {
        /* Healthy. Log only on the down->up transition. */
        if (!connected_flag.exchange(true))
        {
            FSS_LOG_INFO("db", "Database connection '" << conn_name << "' recovered");
        }
        return;
    }
    /* Lost. Log the loss (and a failed retry) only on first detection so a
     * persistently-down connection does not flood the log on every tick. */
    bool was_connected = connected_flag.exchange(false);
    if (was_connected)
    {
        FSS_LOG_WARN("db", "Database connection '" << conn_name << "' lost, attempting reconnect");
    }
    db_disconnect(conn_name);
    bool reconnected = connectOne(conn_name);
    connected_flag.store(reconnected);
    if (reconnected)
    {
        FSS_LOG_INFO("db", "Database connection '" << conn_name << "' reconnected successfully");
    }
    else if (was_connected)
    {
        FSS_LOG_ERROR("db", "Database connection '" << conn_name << "' reconnect failed");
    }
}

void flight_safety_system::server::db_connection::tryReconnectIfNeeded()
{
    {
        std::scoped_lock guard(this->read_lock);
        reconnectOne(read_conn_name, this->read_connected_);
    }
    {
        std::scoped_lock guard(this->write_lock);
        reconnectOne(write_conn_name, this->write_connected_);
    }
}

flight_safety_system::server::db_connection::~db_connection()
{
    {
        std::scoped_lock guard(this->read_lock);
        db_disconnect(read_conn_name);
    }
    {
        std::scoped_lock guard(this->write_lock);
        db_disconnect(write_conn_name);
    }
}

auto flight_safety_system::server::db_connection::getAssetId(const std::string &name) -> uint64_t
{
    std::scoped_lock guard(this->read_lock);
    return db_get_asset_id(read_conn_name, name.c_str());
}

void flight_safety_system::server::db_connection::recordRtt(uint64_t asset_id, uint64_t rtt_ms)
{
    std::scoped_lock guard(this->write_lock);
    db_rtt_create_entry(write_conn_name, asset_id, rtt_ms);
}

void flight_safety_system::server::db_connection::recordStatus(uint64_t asset_id, uint8_t bat_percent,
                                                               uint32_t bat_mah_used, double bat_voltage)
{
    std::scoped_lock guard(this->write_lock);
    db_status_create_entry(write_conn_name, asset_id, bat_percent, bat_mah_used, bat_voltage);
}

void flight_safety_system::server::db_connection::recordSearchStatus(uint64_t asset_id, uint64_t search_id,
                                                                     uint64_t completed, uint64_t total)
{
    std::scoped_lock guard(this->write_lock);
    db_search_status_create_entry(write_conn_name, asset_id, search_id, completed, total);
}

void flight_safety_system::server::db_connection::recordPosition(uint64_t asset_id, double latitude, double longitude,
                                                                 uint32_t altitude)
{
    std::scoped_lock guard(this->write_lock);
    if (altitude > static_cast<uint32_t>(std::numeric_limits<int>::max()))
    {
        FSS_LOG_ERROR("db", "Altitude " << altitude << " exceeds int range; discarding position record for asset "
                                        << asset_id);
        return;
    }
    db_position_create_entry(write_conn_name, asset_id, latitude, longitude, static_cast<int>(altitude));
}

auto flight_safety_system::server::db_connection::getCommand(uint64_t asset_id) -> std::shared_ptr<asset_command>
{
    std::shared_ptr<asset_command> res = nullptr;
    {
        std::scoped_lock guard(this->read_lock);
        // Own the malloc'd C struct via RAII so it is freed on every exit
        // path, including a bad_alloc thrown by make_shared below.
        auto cmd_deleter = [](struct asset_command_s *cmd) -> void {
            if (cmd != nullptr)
            {
                free(cmd->command);
                free(cmd);
            }
        };
        std::unique_ptr<struct asset_command_s, decltype(cmd_deleter)> command(
            db_asset_command_get(read_conn_name, asset_id), cmd_deleter);
        if (command)
        {
            res = std::make_shared<asset_command>(command->dbid, command->timestamp, std::string(command->command),
                                                  command->latitude, command->longitude, command->altitude,
                                                  command->altitude_null == 0);
        }
    }
    return res;
}

auto flight_safety_system::server::db_connection::getSmmSettings(uint64_t asset_id) -> std::shared_ptr<smm_settings>
{
    std::shared_ptr<smm_settings> res = nullptr;
    {
        std::scoped_lock guard(this->read_lock);
        // Own the malloc'd C struct via RAII so it is freed -- and the
        // credential bytes wiped -- on every exit path, including a bad_alloc
        // thrown while constructing the result below.
        auto settings_deleter = [](struct smm_settings_s *settings) -> void {
            if (settings != nullptr)
            {
                free(settings->address);
                if (settings->username != nullptr)
                {
                    explicit_bzero(settings->username, strlen(settings->username));
                    free(settings->username);
                }
                if (settings->password != nullptr)
                {
                    explicit_bzero(settings->password, strlen(settings->password));
                    free(settings->password);
                }
                free(settings);
            }
        };
        std::unique_ptr<struct smm_settings_s, decltype(settings_deleter)> settings(
            db_asset_smm_settings_get(read_conn_name, asset_id), settings_deleter);
        if (settings)
        {
            res = std::make_shared<smm_settings>(
                std::string(settings->address),
                flight_safety_system::secure_string(std::string_view(settings->username)),
                flight_safety_system::secure_string(std::string_view(settings->password)));
        }
    }
    return res;
}

auto flight_safety_system::server::db_connection::getActiveServers() -> std::vector<fss_server_details>
{
    std::vector<fss_server_details> res;
    struct fss_server_s **servers = nullptr;
    int fetch_error = 0;
    {
        std::scoped_lock guard(this->read_lock);
        servers = db_active_fss_servers_get(read_conn_name, &fetch_error);
    }
    if (servers)
    {
        for (size_t i = 0; servers[i] != nullptr; i++)
        {
            res.emplace_back(servers[i]->address, servers[i]->port);
            free(servers[i]->address);
            free(servers[i]);
        }
        db_free_fss_servers(servers);
    }
    /* A mid-cursor failure leaves res holding only the rows read before the
     * error. Discard it: shipping a truncated list to aircraft would drop
     * servers that are actually active. The caller's exception_guard retains
     * the previous good cache. */
    if (fetch_error != 0)
    {
        throw database_error("active FSS server list read failed mid-cursor; partial result discarded");
    }
    return res;
}
