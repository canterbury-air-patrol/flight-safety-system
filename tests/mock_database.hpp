#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "fss-server.hpp"

namespace fss_test {

/* In-memory IDatabase for unit tests. The record* sinks are written by the
 * async db-write-queue worker thread while tests poll/inspect them from the
 * main thread, so those sinks are guarded by records_lock and exposed only via
 * snapshot accessors (getPositions() etc.) that copy under the lock — a bare
 * public vector would race. Commands are held newest-first so getCommand
 * returns the most recently pushed entry, matching the production
 * "ORDER BY timestamp DESC LIMIT 1" semantics. */
class MockDatabase : public flight_safety_system::server::IDatabase {
public:
    std::map<std::string, uint64_t> asset_ids{};
    std::map<uint64_t, std::shared_ptr<flight_safety_system::server::smm_settings>> smm{};
    std::map<uint64_t, std::vector<std::shared_ptr<flight_safety_system::server::asset_command>>> commands{};
    std::vector<flight_safety_system::server::fss_server_details> active_servers{};
    /* When set, getActiveServers returns nullopt, simulating a mid-cursor
     * read failure so tests can exercise the partial-result discard path. */
    bool active_servers_fail{false};
    /* When set, getAssetId returns nullopt regardless of name, simulating a
     * DB read failure so tests can exercise the todo/60 split from a
     * genuinely unknown asset (which stays a 0 lookup miss). */
    bool asset_id_lookup_fail{false};
    /* When set, getSmmSettings returns nullopt, simulating a read failure so
     * tests can exercise the todo/69 split from an asset that genuinely has no
     * settings row (which stays a null pointer inside the optional). */
    bool smm_read_fail{false};
    /* Same, for getCommand and the batched getCommands: nullopt is a failed
     * read, distinct from an asset with no pending command (todo/69). */
    bool command_read_fail{false};

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
    struct recorded_dispatch {
        uint64_t command_dbid;
        uint64_t dispatch_id;
    };
    struct recorded_ack {
        uint64_t asset_id;
        uint64_t dispatch_id;
        uint8_t ack_state;
        uint64_t ack_timestamp;
        uint8_t ack_reason;
    };

    MockDatabase() = default;
    MockDatabase(const MockDatabase &) = delete;
    MockDatabase(MockDatabase &&) = delete;
    auto operator=(const MockDatabase &) -> MockDatabase & = delete;
    auto operator=(MockDatabase &&) -> MockDatabase & = delete;
    ~MockDatabase() override = default;

    auto getAssetId(const std::string &name) -> std::optional<uint64_t> override
    {
        if (asset_id_lookup_fail)
        {
            return std::nullopt;
        }
        auto it = asset_ids.find(name);
        return it == asset_ids.end() ? uint64_t{0} : it->second;
    }

    void recordPosition(uint64_t asset_id, double latitude, double longitude, uint32_t altitude) override
    {
        const std::scoped_lock lock(records_lock);
        positions.push_back({asset_id, latitude, longitude, altitude});
    }

    void recordRtt(uint64_t asset_id, uint64_t rtt_ms) override
    {
        const std::scoped_lock lock(records_lock);
        rtts.push_back({asset_id, rtt_ms});
    }

    void recordStatus(uint64_t asset_id, uint8_t bat_percent, uint32_t bat_mah_used, double bat_voltage) override
    {
        const std::scoped_lock lock(records_lock);
        statuses.push_back({asset_id, bat_percent, bat_mah_used, bat_voltage});
    }

    void recordSearchStatus(uint64_t asset_id, uint64_t search_id, uint64_t completed, uint64_t total) override
    {
        const std::scoped_lock lock(records_lock);
        searches.push_back({asset_id, search_id, completed, total});
    }

    void recordCommandDispatch(uint64_t command_dbid, uint64_t dispatch_id) override
    {
        const std::scoped_lock lock(records_lock);
        dispatches.push_back({command_dbid, dispatch_id});
    }

    void recordCommandAck(uint64_t asset_id, uint64_t dispatch_id, uint8_t ack_state, uint64_t ack_timestamp,
                          uint8_t ack_reason) override
    {
        const std::scoped_lock lock(records_lock);
        acks.push_back({asset_id, dispatch_id, ack_state, ack_timestamp, ack_reason});
    }

    /* Snapshot accessors: copy the sink under the lock so a test can inspect it
     * without racing the write-queue worker that fills it. */
    auto getPositions() const -> std::vector<recorded_pos>
    {
        const std::scoped_lock lock(records_lock);
        return positions;
    }
    auto getRtts() const -> std::vector<recorded_rtt>
    {
        const std::scoped_lock lock(records_lock);
        return rtts;
    }
    auto getStatuses() const -> std::vector<recorded_status>
    {
        const std::scoped_lock lock(records_lock);
        return statuses;
    }
    auto getSearches() const -> std::vector<recorded_search>
    {
        const std::scoped_lock lock(records_lock);
        return searches;
    }
    auto getDispatches() const -> std::vector<recorded_dispatch>
    {
        const std::scoped_lock lock(records_lock);
        return dispatches;
    }
    auto getAcks() const -> std::vector<recorded_ack>
    {
        const std::scoped_lock lock(records_lock);
        return acks;
    }

    auto getCommand(uint64_t asset_id)
        -> std::optional<std::shared_ptr<flight_safety_system::server::asset_command>> override
    {
        if (command_read_fail)
        {
            return std::nullopt;
        }
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

    auto getCommands(const std::vector<uint64_t> &ids)
        -> std::unordered_map<uint64_t, std::shared_ptr<flight_safety_system::server::asset_command>> override
    {
        /* Reuse getCommand's newest-by-timestamp selection so batched and
         * single reads can never disagree; assets with no command are omitted,
         * matching the production "absent == nullptr" contract. */
        std::unordered_map<uint64_t, std::shared_ptr<flight_safety_system::server::asset_command>> res;
        for (uint64_t asset_id : ids)
        {
            auto cmd = getCommand(asset_id);
            if (cmd.has_value() && *cmd != nullptr)
            {
                res[asset_id] = std::move(*cmd);
            }
        }
        return res;
    }

    auto getActiveServers() -> std::optional<std::vector<flight_safety_system::server::fss_server_details>> override
    {
        if (active_servers_fail)
        {
            return std::nullopt;
        }
        return active_servers;
    }

    auto getSmmSettings(uint64_t asset_id)
        -> std::optional<std::shared_ptr<flight_safety_system::server::smm_settings>> override
    {
        smm_reads++;
        if (smm_read_fail)
        {
            return std::nullopt;
        }
        auto it = smm.find(asset_id);
        return it == smm.end() ? nullptr : it->second;
    }

    /* Number of getSmmSettings calls — lets tests assert that send paths
     * use the cache rather than re-reading the database. Atomic because
     * concurrent-identify tests (todo/44) drive refreshSmmSettings() from
     * two threads at once; the smm map itself stays unguarded like the other
     * read-side maps (set up before any threads run, const find() after). */
    std::atomic<int> smm_reads{0};

    auto isConnected() const -> bool override { return true; }
    void tryReconnectIfNeeded() override {}

    void pushCommand(uint64_t asset_id, std::shared_ptr<flight_safety_system::server::asset_command> cmd)
    {
        commands[asset_id].push_back(std::move(cmd));
    }
private:
    /* Written by the async write-queue worker, read via the snapshot accessors
     * above; all access is under records_lock. */
    mutable std::mutex records_lock{};
    std::vector<recorded_rtt> rtts{};
    std::vector<recorded_pos> positions{};
    std::vector<recorded_status> statuses{};
    std::vector<recorded_search> searches{};
    std::vector<recorded_dispatch> dispatches{};
    std::vector<recorded_ack> acks{};
};

} // namespace fss_test
