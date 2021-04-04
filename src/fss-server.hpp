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
    smm_settings(const std::string &t_address, const std::string &t_username, const std::string &t_password) : address(t_address), username(t_username), password(t_password) {};
    std::string getAddress() { return this->address; };
    std::string getUsername() { return this->username; };
    std::string getPassword() { return this->password; };
};

class fss_server_details
{
private:
    std::string address;
    uint16_t port;
public:
    fss_server_details(const std::string &t_address, uint16_t t_port) : address(t_address), port(t_port) {};
    std::string getAddress() { return this->address; };
    uint16_t getPort() { return this->port; };
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
    uint64_t getDBId() { return this->dbid; };
    uint64_t getTimeStamp() { return this->timestamp; };
    transport::fss_asset_command getCommand() { return this->command; };
    double getLatitude() { return this->latitude; };
    double getLongitude() { return this->longitude; };
    uint16_t getAltitude() { return this->altitude; };
};

class db_connection {
private:
    std::mutex db_lock;
public:
    db_connection(std::string host, std::string user, std::string pass, std::string db);
    ~db_connection();
    bool check_asset(std::string asset_name);
    void asset_add_rtt(std::string asset_name, uint64_t rtt);
    void asset_add_status(std::string asset_name, uint8_t bat_percent, uint32_t bat_mah_used);
    void asset_add_search_status(std::string asset_name, uint64_t search_id, uint64_t search_completed, uint64_t search_total);
    void asset_add_position(std::string asset_name, double latitude, double longitude, uint16_t altitude);
    asset_command *asset_get_command(std::string asset_name);
    smm_settings *asset_get_smm_settings(std::string asset_name);
    std::list<fss_server_details *> get_active_fss_servers();
};

class fss_client_rtt {
private:
    uint64_t timestamp;
    uint64_t reqid;
public:
    fss_client_rtt(uint64_t t_timestamp, uint64_t t_reqid) : timestamp(t_timestamp), reqid(t_reqid) {};
    uint64_t getTimeStamp() { return this->timestamp; };
    uint64_t getRequestId() { return this->reqid; };
};

class fss_client: public transport::fss_message_cb {
private:
    bool identified{false};
    bool aircraft{false};
    std::string name{};
    std::list<fss_client_rtt *> outstanding_rtt_requests{};
    std::string getName() { return this->name; };
    uint64_t last_command_send_ts{0};
    uint64_t last_command_dbid{0};
public:
    explicit fss_client(transport::fss_connection *conn);
    virtual ~fss_client();
    virtual void processMessage(std::shared_ptr<transport::fss_message> message) override;
    void sendRTTRequest(std::shared_ptr<transport::fss_message_rtt_request> rtt_req);
    void sendMsg(std::shared_ptr<transport::fss_message> msg);
    void sendSMMSettings();
    void sendCommand();
    void disconnect();
    bool isAircraft() { return this->aircraft; };
};
}
}
