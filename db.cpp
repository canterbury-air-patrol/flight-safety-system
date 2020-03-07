#include "fss-server.hpp"
#include "fss-internal.hpp"

extern "C" {
#include "server-db.h"
}

#include <string>

using namespace flight_safety_system::server;

db_connection::db_connection(std::string host, std::string user, std::string pass, std::string db) : db_lock()
{
    db_connect(host.c_str(), user.c_str(), pass.c_str(), db.c_str());
}

db_connection::~db_connection()
{
    db_disconnect();
}

bool
db_connection::check_asset(std::string asset_name)
{
    this->db_lock.lock();
    uint64_t asset_id = db_get_asset_id(asset_name.c_str());
    this->db_lock.unlock();
    return asset_id != 0;
}

void
db_connection::asset_add_rtt(std::string asset_name, uint64_t delta)
{
    this->db_lock.lock();
    uint64_t asset_id = db_get_asset_id(asset_name.c_str());
    if (asset_id != 0)
    {
        db_rtt_create_entry(asset_id, delta);
    }
    this->db_lock.unlock();
}

void
db_connection::asset_add_status(std::string asset_name, uint8_t bat_percent, uint32_t bat_mah_used)
{
    this->db_lock.lock();
    uint64_t asset_id = db_get_asset_id(asset_name.c_str());
    if (asset_id != 0)
    {
        db_status_create_entry(asset_id, bat_percent, bat_mah_used);
    }
    this->db_lock.unlock();
}

void
db_connection::asset_add_search_status(std::string asset_name, uint64_t search_id, uint64_t search_completed, uint64_t search_total)
{
    this->db_lock.lock();
    uint64_t asset_id = db_get_asset_id(asset_name.c_str());
    if (asset_id != 0)
    {
        db_search_status_create_entry(asset_id, search_id, search_completed, search_total);
    }
    this->db_lock.unlock();
}

void
db_connection::asset_add_position(std::string asset_name, double latitude, double longitude, uint16_t altitude)
{
    this->db_lock.lock();
    uint64_t asset_id = db_get_asset_id(asset_name.c_str());
    if (asset_id != 0)
    {
        db_position_create_entry(asset_id, latitude, longitude, altitude);
    }
    this->db_lock.unlock();
}

asset_command *
db_connection::asset_get_command(std::string asset_name)
{
    this->db_lock.lock();
    uint64_t asset_id = db_get_asset_id(asset_name.c_str());
    if (asset_id != 0)
    {
        struct asset_command_s *command = db_asset_command_get(asset_id);
        if (command)
        {
            this->db_lock.unlock();
            asset_command *res = new asset_command(command->dbid, command->timestamp, std::string(command->command), command->latitude, command->longitude, command->altitude);
            free (command->command);
            free (command);
            return res;
        }
    }
    this->db_lock.unlock();
    return nullptr;
}

smm_settings *
db_connection::asset_get_smm_settings(std::string asset_name)
{
    this->db_lock.lock();
    uint64_t asset_id = db_get_asset_id(asset_name.c_str());
    if (asset_id != 0)
    {
        struct smm_settings_s *settings = db_asset_smm_settings_get(asset_id);
        if (settings)
        {
            this->db_lock.unlock();
            smm_settings *res = new smm_settings(std::string(settings->address), std::string(settings->username), std::string(settings->password));
            free (settings->address);
            free (settings->username);
            free (settings->password);
            free (settings);
            return res;
        }
    }
    this->db_lock.unlock();
    return nullptr;
}

std::list<fss_server_details *>
db_connection::get_active_fss_servers()
{
    std::list<fss_server_details *> res;
    this->db_lock.lock();
    struct fss_server_s **servers = db_active_fss_servers_get();
    this->db_lock.unlock();
    if (servers)
    {
        for(size_t i = 0; servers[i] != NULL; i++)
        {
            res.push_back(new fss_server_details(servers[i]->address, servers[i]->port));
        }
    }
    return res;
}
