#include "fss-internal.hpp"

#include <string>
#include <list>

namespace  flight_safety_system {
class db_connection {
private:
public:
    db_connection(std::string host, std::string user, std::string pass, std::string db);
    ~db_connection();
    void asset_add_rtt(std::string asset_name, uint64_t rtt);
    void asset_add_status(std::string asset_name, uint8_t bat_percent, uint32_t bat_mah_used);
    void asset_add_search_status(std::string asset_name, uint64_t search_id, uint64_t search_completed, uint64_t search_total);
    void asset_add_position(std::string asset_name, double latitude, double longitude, uint16_t altitude);
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
