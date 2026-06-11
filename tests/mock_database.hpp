#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fss-server.hpp"

namespace fss_test {

/* In-memory IDatabase for unit tests. No thread-safety: tests drive the
 * session synchronously. Commands are held newest-first so getCommand
 * returns the most recently pushed entry, matching the production
 * "ORDER BY timestamp DESC LIMIT 1" semantics. */
class MockDatabase : public flight_safety_system::server::IDatabase {
public:
    std::map<std::string, uint64_t> asset_ids{};
    std::map<uint64_t, std::shared_ptr<flight_safety_system::server::smm_settings>> smm{};
    std::map<uint64_t, std::vector<std::shared_ptr<flight_safety_system::server::asset_command>>> commands{};
    std::vector<flight_safety_system::server::fss_server_details> active_servers{};

    struct recorded_rtt {
        uint64_t asset_id;
        uint64_t rtt_ms;
    };
    struct recorded_pos {
        uint64_t asset_id;
        double latitude;
        double longitude;
        uint32_t altitude;
    };
    struct recorded_status {
        uint64_t asset_id;
        uint8_t bat_percent;
        uint32_t bat_mah_used;
        double bat_voltage;
    };
    struct recorded_search {
        uint64_t asset_id;
        uint64_t search_id;
        uint64_t completed;
        uint64_t total;
    };

    std::vector<recorded_rtt> rtts{};
    std::vector<recorded_pos> positions{};
    std::vector<recorded_status> statuses{};
    std::vector<recorded_search> searches{};

    MockDatabase() = default;
    MockDatabase(const MockDatabase &) = delete;
    MockDatabase(MockDatabase &&) = delete;
    auto operator=(const MockDatabase &) -> MockDatabase & = delete;
    auto operator=(MockDatabase &&) -> MockDatabase & = delete;
    ~MockDatabase() override = default;

    auto getAssetId(const std::string &name) -> uint64_t override
    {
        auto it = asset_ids.find(name);
        return it == asset_ids.end() ? 0 : it->second;
    }

    void recordPosition(uint64_t asset_id, double latitude, double longitude, uint32_t altitude) override
    {
        positions.push_back({asset_id, latitude, longitude, altitude});
    }

    void recordRtt(uint64_t asset_id, uint64_t rtt_ms) override { rtts.push_back({asset_id, rtt_ms}); }

    void recordStatus(uint64_t asset_id, uint8_t bat_percent, uint32_t bat_mah_used, double bat_voltage) override
    {
        statuses.push_back({asset_id, bat_percent, bat_mah_used, bat_voltage});
    }

    void recordSearchStatus(uint64_t asset_id, uint64_t search_id, uint64_t completed, uint64_t total) override
    {
        searches.push_back({asset_id, search_id, completed, total});
    }

    auto getCommand(uint64_t asset_id) -> std::shared_ptr<flight_safety_system::server::asset_command> override
    {
        auto it = commands.find(asset_id);
        if (it == commands.end() || it->second.empty())
        {
            return nullptr;
        }
        std::shared_ptr<flight_safety_system::server::asset_command> newest = nullptr;
        for (const auto &cmd : it->second)
        {
            if (newest == nullptr || cmd->getTimeStamp() > newest->getTimeStamp())
            {
                newest = cmd;
            }
        }
        return newest;
    }

    auto getActiveServers() -> std::vector<flight_safety_system::server::fss_server_details> override
    {
        return active_servers;
    }

    auto getSmmSettings(uint64_t asset_id) -> std::shared_ptr<flight_safety_system::server::smm_settings> override
    {
        smm_reads++;
        auto it = smm.find(asset_id);
        return it == smm.end() ? nullptr : it->second;
    }

    /* Number of getSmmSettings calls — lets tests assert that send paths
     * use the cache rather than re-reading the database. */
    int smm_reads{0};

    auto isConnected() const -> bool override { return true; }
    void tryReconnectIfNeeded() override {}

    void pushCommand(uint64_t asset_id, std::shared_ptr<flight_safety_system::server::asset_command> cmd)
    {
        commands[asset_id].push_back(std::move(cmd));
    }
};

} // namespace fss_test
