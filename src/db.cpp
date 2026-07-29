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
    std::string host, int port, std::string user, secure_string pass,
    std::string db) // NOLINT(bugprone-easily-swappable-parameters)
    : read_lock(), write_lock(), host_(std::move(host)), port_(port), user_(std::move(user)), pass_(std::move(pass)),
      db_(std::move(db))
{
    read_connected_ = connectOne(read_conn_name);
    write_connected_ = connectOne(write_conn_name);
}

auto flight_safety_system::server::db_connection::connectOne(const char *conn_name) -> bool
{
    /* pass_ is shared by both the read and write connections, each
     * reconnected from its own thread under its own mutex (see
     * fss-server.hpp), so a local toNulTerminated() copy is used here
     * instead of a cached pointer on pass_ itself — see secure_string's
     * comment. */
    std::string pass_nt = pass_.toNulTerminated();
    return db_connect(conn_name, host_.c_str(), port_, user_.c_str(), pass_nt.c_str(), db_.c_str()) == 1;
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

auto flight_safety_system::server::db_connection::verifySchema() -> bool
{
    int check_error = 0;
    struct schema_problem_s **problems = nullptr;
    {
        std::scoped_lock guard(this->read_lock);
        problems = db_schema_check(read_conn_name, &check_error);
    }

    bool fatal = false;
    if (problems != nullptr)
    {
        for (size_t i = 0; problems[i] != nullptr; i++)
        {
            const struct schema_problem_s *p = problems[i];
            if (p->reason == SCHEMA_PROBLEM_MISSING)
            {
                /* Name every missing column before returning, so one restart
                 * tells the operator the whole gap rather than the first of
                 * several. */
                FSS_LOG_ERROR("db", "Required database column " << p->table << "." << p->column << " is missing");
                fatal = true;
            }
            else if (p->reason == SCHEMA_PROBLEM_MISSING_EXTENSION)
            {
                FSS_LOG_ERROR("db", "Required database extension " << p->column << " is not installed");
                fatal = true;
            }
            else
            {
                /* Not fatal: a column widened past our buffer only bites once
                 * something actually stores an over-long value, at which point
                 * the truncation guards in server-db.pgc drop the row. Refusing
                 * to start over it would turn a benign fss-web migration into a
                 * fleet-wide outage — the failure this check exists to prevent. */
                FSS_LOG_WARN("db", "Database column "
                                       << p->table << "." << p->column << " holds up to " << p->actual_len
                                       << " characters, which does not fit the server's " << (p->buffer_len - 1)
                                       << "-character buffer; over-long values will be dropped");
            }
        }
    }
    db_free_schema_problems(problems);

    if (check_error != 0)
    {
        /* The check could not be completed, so its result says nothing. A
         * database that has just connected but cannot be introspected is a
         * broken deployment; refuse and let the supervisor retry. */
        FSS_LOG_ERROR("db", "Could not verify the database schema");
        return false;
    }
    return !fatal;
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

auto flight_safety_system::server::db_connection::getAssetId(const std::string &name) -> std::optional<uint64_t>
{
    int fetch_error = 0;
    unsigned long long asset_id = 0;
    {
        std::scoped_lock guard(this->read_lock);
        asset_id = db_get_asset_id(read_conn_name, name.c_str(), &fetch_error);
    }
    /* nullopt tells the caller the lookup itself failed (todo/60) -- distinct
     * from 0, which means the query ran fine and found no matching asset. */
    if (fetch_error != 0)
    {
        return std::nullopt;
    }
    return asset_id;
}

void flight_safety_system::server::db_connection::recordRtt(uint64_t asset_id, uint64_t rtt_ms)
{
    std::scoped_lock guard(this->write_lock);
    if (db_rtt_create_entry(write_conn_name, asset_id, rtt_ms) == 0)
    {
        throw database_error("rtt write failed for asset " + std::to_string(asset_id));
    }
}

void flight_safety_system::server::db_connection::recordStatus(uint64_t asset_id, uint8_t bat_percent,
                                                               uint32_t bat_mah_used, double bat_voltage)
{
    std::scoped_lock guard(this->write_lock);
    if (db_status_create_entry(write_conn_name, asset_id, bat_percent, bat_mah_used, bat_voltage) == 0)
    {
        throw database_error("status write failed for asset " + std::to_string(asset_id));
    }
}

void flight_safety_system::server::db_connection::recordSearchStatus(uint64_t asset_id, uint64_t search_id,
                                                                     uint64_t completed, uint64_t total)
{
    std::scoped_lock guard(this->write_lock);
    if (db_search_status_create_entry(write_conn_name, asset_id, search_id, completed, total) == 0)
    {
        throw database_error("search status write failed for asset " + std::to_string(asset_id));
    }
}

void flight_safety_system::server::db_connection::recordCommandDispatch(uint64_t command_dbid, uint64_t dispatch_id)
{
    std::scoped_lock guard(this->write_lock);
    if (db_command_set_dispatch_id(write_conn_name, command_dbid, dispatch_id) == 0)
    {
        throw database_error("command dispatch-id write failed for command " + std::to_string(command_dbid));
    }
}

void flight_safety_system::server::db_connection::recordCommandAck(uint64_t asset_id, uint64_t dispatch_id,
                                                                   uint8_t ack_state, uint64_t ack_timestamp,
                                                                   uint8_t ack_reason)
{
    std::scoped_lock guard(this->write_lock);
    if (db_command_record_ack(write_conn_name, asset_id, dispatch_id, static_cast<int>(ack_state), ack_timestamp,
                              static_cast<int>(ack_reason)) == 0)
    {
        throw database_error("command ack write failed for asset " + std::to_string(asset_id));
    }
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
    if (db_position_create_entry(write_conn_name, asset_id, latitude, longitude, static_cast<int>(altitude)) == 0)
    {
        throw database_error("position write failed for asset " + std::to_string(asset_id));
    }
}

auto flight_safety_system::server::db_connection::getCommand(uint64_t asset_id)
    -> std::optional<std::shared_ptr<asset_command>>
{
    std::shared_ptr<asset_command> res = nullptr;
    int fetch_error = 0;
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
            db_asset_command_get(read_conn_name, asset_id, &fetch_error), cmd_deleter);
        if (command)
        {
            res = std::make_shared<asset_command>(command->dbid, command->timestamp, std::string(command->command),
                                                  command->latitude, command->longitude, command->altitude,
                                                  command->altitude_null == 0);
        }
    }
    /* nullopt tells the caller the read failed, so it must not treat this as
     * "no pending command" and clear one it already holds (todo/69). Called
     * once per identify, so no throttling is needed. */
    if (fetch_error != 0)
    {
        FSS_LOG_ERROR("db", "pending-command read failed for asset " << asset_id);
        return std::nullopt;
    }
    return res;
}

auto flight_safety_system::server::db_connection::getCommands(const std::vector<uint64_t> &asset_ids)
    -> std::unordered_map<uint64_t, std::shared_ptr<asset_command>>
{
    std::unordered_map<uint64_t, std::shared_ptr<asset_command>> res;
    if (asset_ids.empty())
    {
        return res;
    }
    /* At most one entry per requested asset (fewer when some have no pending
     * command); reserve up front so this per-tick poll path never rehashes. */
    res.reserve(asset_ids.size());
    /* db_asset_commands_get takes the C asset-id type (unsigned long long) used
     * throughout server-db.h; copy into it because uint64_t may be a distinct
     * type (unsigned long here) whose pointer will not implicitly convert. */
    std::vector<unsigned long long> ids(asset_ids.begin(), asset_ids.end());
    struct asset_command_row_s **rows = nullptr;
    int fetch_error = 0;
    {
        std::scoped_lock guard(this->read_lock);
        rows = db_asset_commands_get(read_conn_name, ids.data(), ids.size(), &fetch_error);
    }
    if (rows)
    {
        for (size_t i = 0; rows[i] != nullptr; i++)
        {
            struct asset_command_row_s *row = rows[i];
            res[row->asset_id] =
                std::make_shared<asset_command>(row->dbid, row->timestamp, std::string(row->command), row->latitude,
                                                row->longitude, row->altitude, row->altitude_null == 0);
            free(row->command);
            free(row);
        }
        db_free_asset_commands(rows);
    }
    /* A mid-cursor failure leaves res holding only the assets read before the
     * error; the rest are absent (treated as "no pending command"). This is the
     * same outcome the per-asset getCommand path already produces when the read
     * connection is down — both serialise on the one read connection, so a drop
     * fails them all — so the poller's next tick simply retries. No discard. */
    if (fetch_error != 0)
    {
        FSS_LOG_ERROR("db",
                      "batched command read failed mid-cursor; " << res.size() << " asset(s) read before the error");
    }
    return res;
}

auto flight_safety_system::server::db_connection::getSmmSettings(uint64_t asset_id)
    -> std::optional<std::shared_ptr<smm_settings>>
{
    std::shared_ptr<smm_settings> res = nullptr;
    int fetch_error = 0;
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
            db_asset_smm_settings_get(read_conn_name, asset_id, &fetch_error), settings_deleter);
        if (settings)
        {
            res = std::make_shared<smm_settings>(
                std::string(settings->address),
                flight_safety_system::secure_string(std::string_view(settings->username)),
                flight_safety_system::secure_string(std::string_view(settings->password)));
        }
    }
    /* nullopt tells the caller to keep whatever settings it already cached; a
     * null pointer inside the optional is the genuinely-absent answer and does
     * clear the cache (todo/69). */
    if (fetch_error != 0)
    {
        FSS_LOG_ERROR("db", "SMM settings read failed for asset " << asset_id);
        return std::nullopt;
    }
    return res;
}

auto flight_safety_system::server::db_connection::getActiveServers() -> std::optional<std::vector<fss_server_details>>
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
     * servers that are actually active. nullopt tells the caller to keep its
     * previous good list (todo/24). */
    if (fetch_error != 0)
    {
        FSS_LOG_ERROR("db", "active FSS server list read failed mid-cursor; partial result discarded");
        return std::nullopt;
    }
    return res;
}
