#include <fss-transport.hpp>

#include <list>

namespace flight_safety_system {
namespace client {
class fss_client;
class fss_server;

enum connection_status {
CLIENT_CONNECTION_STATUS_UNKNOWN,
CLIENT_CONNECTION_STATUS_CONNECTED_1_SERVER,
CLIENT_CONNECTION_STATUS_CONNECTED_2_OR_MORE,
CLIENT_CONNECTION_STATUS_DISCONNECTED,
};

class fss_client {
private:
    std::string asset_name{""};
    std::list<std::shared_ptr<fss_server>> servers{};
    std::list<std::shared_ptr<fss_server>> reconnect_servers{};
    void notifyConnectionStatus();
    virtual void connectionStatusChange(flight_safety_system::client::connection_status status) {};
public:
    explicit fss_client(const std::string &config_file);
    explicit fss_client() = default;
    virtual ~fss_client() {};
    void connectTo(const std::string &t_address, uint16_t t_port, bool connect);
    virtual void attemptReconnect();
    virtual void sendMsgAll(flight_safety_system::transport::fss_message *msg);
    virtual auto getAssetName() -> std::string { return this->asset_name; };
    virtual void serverRequiresReconnect(const std::shared_ptr<fss_server> &server);
    virtual void updateServers(flight_safety_system::transport::fss_message_server_list *msg);
    virtual void handleCommand(flight_safety_system::transport::fss_message_asset_command *msg) {};
    virtual void handlePositionReport(flight_safety_system::transport::fss_message_position_report *msg) {};
    virtual void handleSMMSettings(flight_safety_system::transport::fss_message_smm_settings *msg) {};
};

class fss_server: public flight_safety_system::transport::fss_message_cb {
protected:
    fss_client *client;
    std::string address;
    uint16_t port;
    uint64_t last_tried{0};
    uint64_t retry_count{0};
    static constexpr uint64_t retry_delay_start = 1000;
    static constexpr uint64_t retry_delay_cap = 30000;
    uint64_t retry_delay{retry_delay_start};
public:
    fss_server(fss_client *t_client, std::string t_address, uint16_t t_port, bool connect);
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
    virtual auto reconnect() -> bool;
    virtual auto connected() -> bool { return this->conn != nullptr; };
    virtual auto getClient() -> fss_client * { return this->client; };
    virtual void sendIdentify();
};
}
}
