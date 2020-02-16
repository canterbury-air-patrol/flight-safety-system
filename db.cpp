#include "fss-server.hpp"
#include "fss-internal.hpp"

extern "C" {
#include "server-db.h"
};

#include <string>

db_connection::db_connection(std::string host, std::string user, std::string pass, std::string db)
{
    db_connect(host.c_str(), user.c_str(), pass.c_str(), db.c_str());
}

db_connection::~db_connection()
{
    db_disconnect();
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
