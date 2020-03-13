#include <fss.hpp>

#include <list>

namespace flight_safety_system {
namespace client {
class fss_client;
class fss_server;

class fss_client {
protected:
    std::string asset_name{""};
    std::list<fss_server *> servers{};
    std::list<fss_server *> reconnect_servers{};
public:
    explicit fss_client(const std::string &config_file);
    virtual ~fss_client();
    void connectTo(const std::string &t_address, uint16_t t_port);
    virtual void attemptReconnect();
    virtual void sendMsgAll(flight_safety_system::transport::fss_message *msg);
    virtual std::string getAssetName() { return this->asset_name; };
    virtual void serverRequiresReconnect(fss_server *server);
    virtual void updateServers(flight_safety_system::transport::fss_message_server_list *msg);
};

class fss_server: public flight_safety_system::transport::fss_message_cb {
protected:
    fss_client *client;
    std::string address;
    uint16_t port;
    uint64_t last_tried{0};
    uint64_t retry_count{0};
public:
    fss_server(fss_client *t_client, flight_safety_system::transport::fss_connection *t_conn, const std::string &t_address, uint16_t t_port);
    fss_server(const fss_server &other) : flight_safety_system::transport::fss_message_cb(other.conn), client(other.client), address(other.address), port(other.port) {};
    fss_server& operator=(const fss_server& other)
    {
        if (this != &other)
        {
            this->client = other.client;
            this->address = other.address;
            this->port = other.port;
            this->last_tried = 0;
            this->retry_count = 0;
        }
        return *this;
    }
    virtual ~fss_server();
    virtual void processMessage(flight_safety_system::transport::fss_message *message) override;
    virtual std::string getAddress() { return this->address; };
    virtual uint16_t getPort() { return this->port; };
    virtual bool reconnect();
    virtual fss_client *getClient() { return this->client; };
};
}
}
