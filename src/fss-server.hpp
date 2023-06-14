#include "fss-transport.hpp"

#include <string>
#include <list>
#include <mutex>

namespace  flight_safety_system {
namespace server {
class smm_settings {
private:
    std::string address;
    std::string username;
    std::string password;
public:
    smm_settings(std::string t_address, std::string t_username, std::string t_password);
    auto getAddress() -> std::string;
    auto getUsername() -> std::string;
    auto getPassword() -> std::string;
};

class fss_server_details
{
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
    transport::fss_asset_command command;
    double latitude;
    double longitude;
    uint16_t altitude;
public:
    asset_command(uint64_t t_dbid, uint64_t t_timestamp, const std::string &t_cmd, double t_latitude, double t_longitude, uint16_t t_altitude);
    auto getDBId() -> uint64_t;
    auto getTimeStamp() -> uint64_t;
    auto getCommand() -> transport::fss_asset_command;
    auto getLatitude() -> double;
    auto getLongitude() -> double;
    auto getAltitude() -> uint16_t;
};

class db_connection {
private:
    std::mutex db_lock;
public:
    db_connection(const std::string &host, const std::string &user, const std::string &pass, const std::string &db);
    db_connection(db_connection&) = delete;
    db_connection(db_connection&&) = delete;
    auto operator=(db_connection&) -> db_connection& = delete;
    auto operator=(db_connection&&) -> db_connection& = delete;
    ~db_connection();
    auto check_asset(const std::string &asset_name) -> bool;
    void asset_add_rtt(const std::string &asset_name, uint64_t rtt);
    void asset_add_status(const std::string &asset_name, uint8_t bat_percent, uint32_t bat_mah_used, double bat_voltage);
    void asset_add_search_status(const std::string &asset_name, uint64_t search_id, uint64_t search_completed, uint64_t search_total);
    void asset_add_position(const std::string &asset_name, double latitude, double longitude, uint16_t altitude);
    auto asset_get_command(const std::string &asset_name) -> std::shared_ptr<asset_command>;
    auto asset_get_smm_settings(const std::string &asset_name) -> std::shared_ptr<smm_settings>;
    auto get_active_fss_servers() -> std::list<std::shared_ptr<fss_server_details>>;
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

class fss_client: public transport::fss_message_cb {
private:
    bool identified{false};
    bool aircraft{false};
    std::string name{};
    std::list<std::shared_ptr<fss_client_rtt>> outstanding_rtt_requests{};
    auto getName() -> std::string;
    uint64_t last_command_send_ts{0};
    uint64_t last_command_dbid{0};
public:
    explicit fss_client(std::shared_ptr<transport::fss_connection> conn);
    fss_client(fss_client&) = delete;
    fss_client(fss_client&&) = delete;
    auto operator=(fss_client&) -> fss_client& = delete;
    auto operator=(fss_client&&) -> fss_client& = delete;
    ~fss_client() override;
    void processMessage(std::shared_ptr<transport::fss_message> message) override;
    void sendRTTRequest(const std::shared_ptr<transport::fss_message_rtt_request> &rtt_req);
    void sendSMMSettings();
    void sendCommand();
    auto isAircraft() -> bool;
};
} // namespace server
} // namespace flight_safety_system
