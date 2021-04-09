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
    smm_settings(std::string t_address, std::string t_username, std::string t_password) : address(std::move(t_address)), username(std::move(t_username)), password(std::move(t_password)) {};
    auto getAddress() -> std::string { return this->address; };
    auto getUsername() -> std::string { return this->username; };
    auto getPassword() -> std::string { return this->password; };
};

class fss_server_details
{
private:
    std::string address;
    uint16_t port;
public:
    fss_server_details(std::string t_address, uint16_t t_port) : address(std::move(t_address)), port(t_port) {};
    auto getAddress() -> std::string { return this->address; };
    auto getPort() -> uint16_t { return this->port; };
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
    asset_command(uint64_t t_dbid, uint64_t t_timestamp, const std::string &t_cmd, double t_latitude, double t_longitude, uint16_t t_altitude) : dbid(t_dbid), timestamp(t_timestamp), command(transport::asset_command_unknown), latitude(t_latitude), longitude(t_longitude), altitude(t_altitude) {
        if (t_cmd == "RTL") {
            this->command = transport::asset_command_rtl;
        } else if (t_cmd == "HOLD") {
            this->command = transport::asset_command_hold;
        } else if (t_cmd == "GOTO") {
            this->command = transport::asset_command_goto;
        } else if (t_cmd == "RON") {
            this->command = transport::asset_command_resume;
        } else if (t_cmd == "DISARM") {
            this->command = transport::asset_command_disarm;
        } else if (t_cmd == "ALT") {
            this->command = transport::asset_command_altitude;
        } else if (t_cmd == "TERM") {
            this->command = transport::asset_command_terminate;
        } else if (t_cmd == "MAN") {
            this->command = transport::asset_command_manual;
        }
    };
    auto getDBId() -> uint64_t { return this->dbid; };
    auto getTimeStamp() -> uint64_t { return this->timestamp; };
    auto getCommand() -> transport::fss_asset_command { return this->command; };
    auto getLatitude() -> double { return this->latitude; };
    auto getLongitude() -> double { return this->longitude; };
    auto getAltitude() -> uint16_t { return this->altitude; };
};

class db_connection {
private:
    std::mutex db_lock;
public:
    db_connection(const std::string &host, const std::string &user, const std::string &pass, const std::string &db);
    ~db_connection();
    auto check_asset(const std::string &asset_name) -> bool;
    void asset_add_rtt(const std::string &asset_name, uint64_t rtt);
    void asset_add_status(const std::string &asset_name, uint8_t bat_percent, uint32_t bat_mah_used);
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
    fss_client_rtt(uint64_t t_timestamp, uint64_t t_reqid) : timestamp(t_timestamp), reqid(t_reqid) {};
    auto getTimeStamp() -> uint64_t { return this->timestamp; };
    auto getRequestId() -> uint64_t { return this->reqid; };
};

class fss_client: public transport::fss_message_cb {
private:
    bool identified{false};
    bool aircraft{false};
    std::string name{};
    std::list<std::shared_ptr<fss_client_rtt>> outstanding_rtt_requests{};
    auto getName() -> std::string { return this->name; };
    uint64_t last_command_send_ts{0};
    uint64_t last_command_dbid{0};
public:
    explicit fss_client(std::shared_ptr<transport::fss_connection> conn);
    ~fss_client() override;
    void processMessage(std::shared_ptr<transport::fss_message> message) override;
    void sendRTTRequest(std::shared_ptr<transport::fss_message_rtt_request> rtt_req);
    void sendMsg(const std::shared_ptr<transport::fss_message> &msg);
    void sendSMMSettings();
    void sendCommand();
    void disconnect();
    auto isAircraft() -> bool { return this->aircraft; };
};
}
}
