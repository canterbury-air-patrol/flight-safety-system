#pragma once

#include "fss.hpp"
#include "fss-transport.hpp"
#include "db-write-queue.hpp"
#include "rate-limiter.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <list>
#include <mutex>
#include <vector>

namespace flight_safety_system {
namespace server {

constexpr int command_poll_ms = 100;

class smm_settings {
private:
    std::string address;
    secure_string username;
    secure_string password;
public:
    smm_settings(std::string t_address, secure_string t_username, secure_string t_password);
    auto getAddress() -> std::string;
    auto getUsername() -> const secure_string &;
    auto getPassword() -> const secure_string &;
};

class fss_server_details {
private:
    std::string address;
    uint16_t port;
public:
    fss_server_details(std::string t_address, uint16_t t_port);
    auto getAddress() -> std::string;
    auto getPort() -> uint16_t;
};

class asset_command {
private:
    uint64_t dbid;
    uint64_t timestamp;
    transport::fss_asset_command command{transport::fss_asset_command::asset_command_unknown};
    double latitude;
    double longitude;
    uint32_t altitude;
public:
    asset_command(uint64_t t_dbid, uint64_t t_timestamp, const std::string &t_cmd, double t_latitude,
                  double t_longitude, uint32_t t_altitude);
    auto getDBId() -> uint64_t;
    auto getTimeStamp() -> uint64_t;
    auto getCommand() -> transport::fss_asset_command;
    auto getLatitude() -> double;
    auto getLongitude() -> double;
    auto getAltitude() -> uint32_t;
};

/* Pure-virtual database seam: lets ClientSession be unit-tested against
 * an in-memory mock without a live PostgreSQL. db_connection implements
 * it in production; tests/mock_database.hpp implements it in Catch2. */
class IDatabase {
public:
    IDatabase() = default;
    IDatabase(const IDatabase &) = delete;
    IDatabase(IDatabase &&) = delete;
    auto operator=(const IDatabase &) -> IDatabase & = delete;
    auto operator=(IDatabase &&) -> IDatabase & = delete;
    virtual ~IDatabase() = default;
    virtual auto getAssetId(const std::string &name) -> uint64_t = 0;
    virtual void recordPosition(uint64_t asset_id, double latitude, double longitude, uint32_t altitude) = 0;
    virtual void recordRtt(uint64_t asset_id, uint64_t rtt_ms) = 0;
    virtual void recordStatus(uint64_t asset_id, uint8_t bat_percent, uint32_t bat_mah_used, double bat_voltage) = 0;
    virtual void recordSearchStatus(uint64_t asset_id, uint64_t search_id, uint64_t completed, uint64_t total) = 0;
    virtual auto getCommand(uint64_t asset_id) -> std::shared_ptr<asset_command> = 0;
    virtual auto getActiveServers() -> std::vector<fss_server_details> = 0;
    virtual auto getSmmSettings(uint64_t asset_id) -> std::shared_ptr<smm_settings> = 0;
    virtual auto isConnected() const -> bool = 0;
    virtual void tryReconnectIfNeeded() = 0;
};

class db_connection : public IDatabase {
private:
    /* Two independent ECPG connections so a stalled telemetry write cannot
     * block a command/config read. Each connection is guarded by its own
     * mutex and therefore only ever used by one thread at a time — the
     * thread-safe usage pattern ECPG documents. read_lock covers getAssetId,
     * getCommand, getActiveServers and getSmmSettings; write_lock covers the
     * record* telemetry inserts. */
    std::mutex read_lock;
    std::mutex write_lock;
    /* Read without holding either mutex by isConnected(), and written from
     * reconnectOne() under the matching mutex, so they must be atomic to
     * avoid a data race. */
    std::atomic<bool> read_connected_{false};
    std::atomic<bool> write_connected_{false};
    std::string host_;
    int port_{5432};
    std::string user_;
    std::string pass_;
    std::string db_;
    auto connectOne(const char *conn_name) -> bool;
    void reconnectOne(const char *conn_name, std::atomic<bool> &connected_flag);
public:
    /* ECPG connection names, exposed so tests can target a specific
     * connection (e.g. force one closed) without duplicating the literals. */
    static constexpr const char *read_conn_name = "fss_read";
    static constexpr const char *write_conn_name = "fss_write";
    db_connection(std::string host, int port, std::string user, std::string pass, std::string db);
    db_connection(db_connection &) = delete;
    db_connection(db_connection &&) = delete;
    auto operator=(db_connection &) -> db_connection & = delete;
    auto operator=(db_connection &&) -> db_connection & = delete;
    ~db_connection() override;
    auto getAssetId(const std::string &name) -> uint64_t override;
    void recordPosition(uint64_t asset_id, double latitude, double longitude, uint32_t altitude) override;
    void recordRtt(uint64_t asset_id, uint64_t rtt_ms) override;
    void recordStatus(uint64_t asset_id, uint8_t bat_percent, uint32_t bat_mah_used, double bat_voltage) override;
    void recordSearchStatus(uint64_t asset_id, uint64_t search_id, uint64_t completed, uint64_t total) override;
    auto getCommand(uint64_t asset_id) -> std::shared_ptr<asset_command> override;
    auto getActiveServers() -> std::vector<fss_server_details> override;
    auto getSmmSettings(uint64_t asset_id) -> std::shared_ptr<smm_settings> override;
    auto isConnected() const -> bool override;
    void tryReconnectIfNeeded() override;
};

class fss_client_rtt {
private:
    uint64_t timestamp;
    uint64_t reqid;
public:
    fss_client_rtt(uint64_t t_timestamp, uint64_t t_reqid);
    auto getTimeStamp() -> uint64_t;
    auto getRequestId() -> uint64_t;
};

class fss_client;

class fss_client_handler {
public:
    virtual ~fss_client_handler() = default;
    virtual void clientDisconnected(fss_client *client) = 0;
    virtual void broadcastMsg(const std::shared_ptr<transport::fss_message> &msg, fss_client *except = nullptr) = 0;
};

class fss_client : public transport::fss_message_cb {
private:
    std::atomic<bool> identified{false};
    std::atomic<bool> aircraft{false};
    std::atomic<bool> version_received{false};
    std::atomic<uint64_t> expected_seq{0};
    std::atomic<uint64_t> cached_asset_id{0};
    std::shared_ptr<asset_command> pending_command{nullptr};
    std::string name{};
    std::mutex client_lock{};
    std::list<std::shared_ptr<fss_client_rtt>> outstanding_rtt_requests{};
    auto getName() -> std::string;
    uint64_t last_command_send_ts{0};
    uint64_t last_command_dbid{0};
    bool liveness_active{false};
    uint64_t last_rtt_response_time{0};
    uint64_t client_timeout_ms{30000};
    IDatabase *dbc;
    std::shared_ptr<db_write_queue> writer;
    fss_client_handler *client_handler;
    std::shared_ptr<IClock> clock{std::make_shared<WallClock>()};
    rate_limiter msg_rate{100, 20};
    uint64_t rate_limit_rejects{0};
    uint64_t last_rate_limit_log_ms{0};
public:
    fss_client(std::shared_ptr<transport::fss_connection> conn, IDatabase *t_dbc,
               std::shared_ptr<db_write_queue> t_writer, fss_client_handler *t_handler);
    fss_client(fss_client &) = delete;
    fss_client(fss_client &&) = delete;
    auto operator=(fss_client &) -> fss_client & = delete;
    auto operator=(fss_client &&) -> fss_client & = delete;
    ~fss_client() override;
    /* Wire this client as the connection's message handler. Call only after
     * per-client config (timeout, rate limits) is set; see the constructor. */
    void activate();
    void processMessage(std::shared_ptr<transport::fss_message> message) override;
    void sendRTTRequest(const std::shared_ptr<transport::fss_message_rtt_request> &rtt_req);
    void sendSMMSettings();
    void sendCommand();
    auto isAircraft() -> bool;
    auto getCachedAssetId() -> uint64_t { return this->cached_asset_id.load(); }
    void setPendingCommand(std::shared_ptr<asset_command> cmd);
    void setClock(std::shared_ptr<IClock> t_clock);
    void setTimeoutMs(uint64_t ms);
    void setRateLimits(uint64_t capacity, uint64_t refill_per_s); // must be called before any messages are processed
    auto isTimedOut() -> bool;
};
} // namespace server
} // namespace flight_safety_system
