#include <fss-transport.hpp>

#include <list>

#include <gnutls/gnutls.h>
#include <gnutls/gnutlsxx.h>

namespace flight_safety_system {
namespace transport_ssl {
class fss_connection : public flight_safety_system::transport::fss_connection {
private:
    gnutls::certificate_credentials credentials{};
    std::string ca_file;
    std::string private_key_file;
    std::string public_key_file;
protected:
    bool usable{false};
    auto sendSessionMsg(gnutls::session &session, const std::shared_ptr<flight_safety_system::transport::buf_len> &bl) -> bool;
    auto recvSessionBytes(gnutls::session &session, void *bytes, size_t max_bytes) -> ssize_t;
    void setupSession(gnutls::session &session);
public:
    fss_connection(std::string t_ca, std::string t_private_key, std::string t_public_key) : flight_safety_system::transport::fss_connection(), ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)), public_key_file(std::move(t_public_key)) {};
    fss_connection(int t_fd, std::string t_ca, std::string t_private_key, std::string t_public_key);
    fss_connection(fss_connection&) = delete;
    fss_connection(fss_connection&&) = delete;
    auto operator=(fss_connection&) -> fss_connection& = delete;
    auto operator=(fss_connection&&) -> fss_connection& = delete;
    virtual ~fss_connection();
};

class fss_connection_client : public fss_connection {
private:
    gnutls::client_session session;
    std::string hostname{};
protected:
    auto setupSSL() -> bool;
    auto sendMsg(const std::shared_ptr<flight_safety_system::transport::buf_len> &bl) -> bool override;
    auto recvBytes(void *bytes, size_t max_bytes) -> ssize_t override;
public:
    fss_connection_client(std::string t_ca, std::string t_private_key, std::string t_public_key) : fss_connection(std::move(t_ca), std::move(t_private_key), std::move(t_public_key)), session{} {};
    auto connectTo(const std::string &address, uint16_t port) -> bool override;
    ~fss_connection_client();
};


class fss_connection_server : public fss_connection {
private:
    std::list<std::string> possible_names{};
    gnutls::server_session session{};
protected:
    auto setupSSL() -> bool;
    auto sendMsg(const std::shared_ptr<flight_safety_system::transport::buf_len> &bl) -> bool override;
    auto recvBytes(void *bytes, size_t max_bytes) -> ssize_t override;
public:
    fss_connection_server(int t_fd, std::string t_ca, std::string t_private_key, std::string t_public_key);
    ~fss_connection_server();
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
    fss_listen(uint16_t t_port, flight_safety_system::transport::fss_connect_cb t_cb, std::string t_ca, std::string t_private_key, std::string t_public_key) : flight_safety_system::transport::fss_listen(t_port, t_cb), ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)), public_key_file(std::move(t_public_key)) {};
};
} // namespace transport_ssl
} // namespace flight_safety_system