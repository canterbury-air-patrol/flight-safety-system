#include <fss-transport.hpp>

#include <list>

namespace gnutls {
    class certificate_credentials;
    class session;
} // namespace gnutls

namespace flight_safety_system {
namespace transport_ssl {
class fss_connection : public flight_safety_system::transport::fss_connection {
private:
    std::unique_ptr<gnutls::certificate_credentials> credentials;
    std::string ca_file;
    std::string private_key_file;
    std::string public_key_file;
protected:
    std::unique_ptr<gnutls::session> session{nullptr};
    bool usable{false};
    auto sendMsg(const std::shared_ptr<flight_safety_system::transport::buf_len> &bl) -> bool override;
    auto recvBytes(void *bytes, size_t max_bytes) -> ssize_t override;
    void setupSession();
public:
    fss_connection(std::string t_ca, std::string t_private_key, std::string t_public_key);
    fss_connection(int t_fd, std::string t_ca, std::string t_private_key, std::string t_public_key);
    fss_connection(fss_connection&) = delete;
    fss_connection(fss_connection&&) = delete;
    auto operator=(fss_connection&) -> fss_connection& = delete;
    auto operator=(fss_connection&&) -> fss_connection& = delete;
    ~fss_connection() override;
};

class fss_connection_client : public fss_connection {
private:
    std::string hostname{};
protected:
    auto setupSSL() -> bool;
public:
    fss_connection_client(std::string t_ca, std::string t_private_key, std::string t_public_key);
    fss_connection_client(fss_connection_client &) = delete;
    fss_connection_client(fss_connection_client &&) = delete;
    auto operator=(fss_connection_client &) -> fss_connection& = delete;
    auto operator=(fss_connection_client &&) -> fss_connection& = delete;
    ~fss_connection_client() override;
    auto connectTo(const std::string &address, uint16_t port) -> bool override;
};


class fss_connection_server : public fss_connection {
private:
    std::list<std::string> possible_names{};
protected:
    auto setupSSL() -> bool;
public:
    fss_connection_server(int t_fd, std::string t_ca, std::string t_private_key, std::string t_public_key);
    fss_connection_server(fss_connection_server&) = delete;
    fss_connection_server(fss_connection_server&&) = delete;
    auto operator=(fss_connection_server&) -> fss_connection_server& = delete;
    auto operator=(fss_connection_server&&) -> fss_connection_server& = delete;
    ~fss_connection_server() override;
    auto getClientNames() -> std::list<std::string> override;
};

class fss_listen : public flight_safety_system::transport::fss_listen {
private:
    std::string ca_file;
    std::string private_key_file;
    std::string public_key_file;
protected:
    auto newConnection(int fd) -> std::shared_ptr<flight_safety_system::transport::fss_connection> override;
public:
    fss_listen(uint16_t t_port, flight_safety_system::transport::fss_connect_cb t_cb, std::string t_ca, std::string t_private_key, std::string t_public_key);
};
} // namespace transport_ssl
} // namespace flight_safety_system
