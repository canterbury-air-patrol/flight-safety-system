#include "fss-server.hpp"
#include "fss-internal.hpp"

extern "C" {
#include "server-db.h"
}

#include <string>

db_connection::db_connection(std::string host, std::string user, std::string pass, std::string db)
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
    uint64_t asset_id = db_get_asset_id(asset_name.c_str());
    return asset_id != 0;
}

void
db_connection::asset_add_rtt(std::string asset_name, uint64_t delta)
{
    uint64_t asset_id = db_get_asset_id(asset_name.c_str());
    if (asset_id != 0)
    {
        db_rtt_create_entry(asset_id, delta);
    }
}

void
db_connection::asset_add_status(std::string asset_name, uint8_t bat_percent, uint32_t bat_mah_used)
{
    uint64_t asset_id = db_get_asset_id(asset_name.c_str());
    if (asset_id != 0)
    {
        db_status_create_entry(asset_id, bat_percent, bat_mah_used);
    }
}

void
db_connection::asset_add_search_status(std::string asset_name, uint64_t search_id, uint64_t search_completed, uint64_t search_total)
{
    uint64_t asset_id = db_get_asset_id(asset_name.c_str());
    if (asset_id != 0)
    {
        db_search_status_create_entry(asset_id, search_id, search_completed, search_total);
    }
}

void
db_connection::asset_add_position(std::string asset_name, double latitude, double longitude, uint16_t altitude)
{
    uint64_t asset_id = db_get_asset_id(asset_name.c_str());
    if (asset_id != 0)
    {
        db_position_create_entry(asset_id, latitude, longitude, altitude);
    }
}

smm_settings *
db_connection::asset_get_smm_settings(std::string asset_name)
{
    uint64_t asset_id = db_get_asset_id(asset_name.c_str());
    if (asset_id != 0)
    {
        struct smm_settings_s *settings = db_asset_smm_settings_get(asset_id);
        if (settings)
        {
            smm_settings *res = new smm_settings(std::string(settings->address), std::string(settings->username), std::string(settings->password));
            free (settings->address);
            free (settings->username);
            free (settings->password);
            free (settings);
            return res;
        }
    }
    return nullptr;
}

std::list<fss_server_details *>
db_connection::get_active_fss_servers()
{
    std::list<fss_server_details *> res;
    struct fss_server_s **servers = db_active_fss_servers_get();
    if (servers)
    {
        for(size_t i = 0; servers[i] != NULL; i++)
        {
            res.push_back(new fss_server_details(servers[i]->address, servers[i]->port));
        }
    }
    return res;
}
