#include <fss-transport-ssl.hpp>

#include <list>
#include <memory>

namespace flight_safety_system {
namespace client_ssl {

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
    std::string ca_file{""};
    std::string private_key_file{""};
    std::string public_key_file{""};
    std::list<std::shared_ptr<flight_safety_system::client_ssl::fss_server>> servers{};
    std::list<std::shared_ptr<flight_safety_system::client_ssl::fss_server>> reconnect_servers{};
    void notifyConnectionStatus();
    virtual void connectionStatusChange(flight_safety_system::client_ssl::connection_status status __attribute__((unused))) {};
protected:
    void setAssetName(std::string t_asset_name);
    void addServer(const std::shared_ptr<fss_server> &server);
public:
    explicit fss_client(const std::string &config_file);
    explicit fss_client() = default;
    explicit fss_client(std::string t_ca, std::string t_private_key, std::string t_public_key);
    fss_client(fss_client&) = delete;
    fss_client(fss_client&&) = delete;
    auto operator=(fss_client&) -> fss_client& = delete;
    auto operator=(fss_client&&) -> fss_client& = delete;
    virtual ~fss_client() = default;
    virtual void connectTo(const std::string &t_address, uint16_t t_port, bool connect);
    virtual void attemptReconnect();
    virtual void disconnect();
    virtual void sendMsgAll(const std::shared_ptr<flight_safety_system::transport::fss_message> &msg);
    virtual auto getAssetName() -> std::string { return this->asset_name; };
    virtual void serverRequiresReconnect(fss_server *server);
    virtual void updateServers(const std::shared_ptr<flight_safety_system::transport::fss_message_server_list> &msg);
    virtual void handleCommand(const std::shared_ptr<flight_safety_system::transport::fss_message_asset_command> &msg __attribute__((unused)));
    virtual void handlePositionReport(const std::shared_ptr<flight_safety_system::transport::fss_message_position_report> &msg __attribute__((unused)));
    virtual void handleSMMSettings(const std::shared_ptr<flight_safety_system::transport::fss_message_smm_settings> &msg __attribute__((unused)));
};

class fss_server: public flight_safety_system::transport::fss_message_cb {
private:
    fss_client *client;
    std::string address;
    uint16_t port;
    std::string ca_file{""};
    std::string private_key_file{""};
    std::string public_key_file{""};
    uint64_t last_tried{0};
    uint64_t retry_count{0};
    static constexpr uint64_t retry_delay_start = 1000;
    static constexpr uint64_t retry_delay_cap = 30000;
    uint64_t retry_delay{retry_delay_start};
protected:
    auto reconnect_to() -> bool;
public:
    fss_server(fss_client *t_client, std::string t_address, uint16_t t_port, std::string t_ca, std::string t_private_key, std::string t_public_key);
    fss_server(fss_server &other) = delete;
    fss_server(fss_server&&) = delete;
    auto operator=(fss_server&) -> fss_server& = delete;
    auto operator=(fss_server&&) -> fss_server& = delete;
    virtual ~fss_server() = default;
    void processMessage(std::shared_ptr<flight_safety_system::transport::fss_message> message) override;
    virtual auto getAddress() -> std::string;
    virtual auto getPort() -> uint16_t;
    virtual auto reconnect() -> bool;
    virtual auto getClient() -> fss_client *;
    virtual void sendIdentify();
};


} // namespace client_ssl
} // namespace flight_safety_system