#include "fss-internal.hpp"

#include <string>
#include <list>

namespace  flight_safety_system {
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
    fss_server_details(std::string t_address, uint16_t t_port) : address(t_address), port(t_port) {};
    std::string getAddress() { return this->address; };
    uint16_t getPort() { return this->port; };
};

class db_connection {
private:
public:
    db_connection(std::string host, std::string user, std::string pass, std::string db);
    ~db_connection();
    bool check_asset(std::string asset_name);
    void asset_add_rtt(std::string asset_name, uint64_t rtt);
    void asset_add_status(std::string asset_name, uint8_t bat_percent, uint32_t bat_mah_used);
    void asset_add_search_status(std::string asset_name, uint64_t search_id, uint64_t search_completed, uint64_t search_total);
    void asset_add_position(std::string asset_name, double latitude, double longitude, uint16_t altitude);
    smm_settings *asset_get_smm_settings(std::string asset_name);
    std::list<fss_server_details *> get_active_fss_servers();
};

class fss_client_rtt {
private:
    uint64_t timestamp;
    uint64_t reqid;
public:
    explicit fss_client_rtt(uint64_t id);
    uint64_t getTimeStamp() { return this->timestamp; };
    uint64_t getRequestId() { return this->reqid; };
};

class fss_client: public fss_message_cb {
private:
    bool identified;
    std::string name;
    std::list<fss_client_rtt *> outstanding_rtt_requests;
public:
    explicit fss_client(fss_connection *conn);
    virtual ~fss_client();
    virtual void processMessage(fss_message *message) override;
};

}
