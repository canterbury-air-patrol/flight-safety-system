#include <fss-transport-ssl.hpp>
#include <fss-client.hpp>

#include <list>

namespace flight_safety_system {
namespace client_ssl {

class fss_client : public flight_safety_system::client::fss_client {
private:
    std::string ca_file{""};
    std::string private_key_file{""};
    std::string public_key_file{""};
public:
    explicit fss_client(const std::string &config_file);
    explicit fss_client() = default;
    fss_client(fss_client&) = delete;
    fss_client(fss_client&&) = delete;
    auto operator=(fss_client&) -> fss_client& = delete;
    auto operator=(fss_client&&) -> fss_client& = delete;
    virtual ~fss_client() = default;
    void connectTo(const std::string &t_address, uint16_t t_port, bool connect) override;
};

class fss_server: public flight_safety_system::client::fss_server {
private:
    std::string ca_file{""};
    std::string private_key_file{""};
    std::string public_key_file{""};
public:
    fss_server(fss_client *t_client, std::string t_address, uint16_t t_port, bool connect, std::string t_ca, std::string t_private_key, std::string t_public_key) : flight_safety_system::client::fss_server(t_client, t_address, t_port, connect), ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)), public_key_file(std::move(t_public_key)) {};
    fss_server(fss_server &other) : flight_safety_system::client::fss_server(other), ca_file(other.ca_file), private_key_file(other.private_key_file), public_key_file(other.public_key_file) {};
    fss_server(fss_server&&) = delete;
    auto operator=(fss_server&) -> fss_server& = delete;
    auto operator=(fss_server&&) -> fss_server& = delete;
    ~fss_server() override = default;
    auto reconnect_to() -> bool override;
};


}; // namespace client_ssl
}; // namespace flight_safety_system