#include <fss-transport.hpp>

#include <gnutls/gnutls.h>
#include <gnutls/gnutlsxx.h>

namespace flight_safety_system {
namespace transport_ssl {
class fss_connection : public flight_safety_system::transport::fss_connection {
protected:
    bool usable{false};
    gnutls::session session;
    gnutls::certificate_credentials credentials{};
    std::string ca_file;
    std::string private_key_file;
    std::string public_key_file;
    auto setupSSL() -> bool;
    auto sendMsg(const std::shared_ptr<flight_safety_system::transport::buf_len> &bl) -> bool override;
    auto recvBytes(void *bytes, size_t max_bytes) -> ssize_t override;
public:
    fss_connection(unsigned int t_session_flags, std::string t_ca, std::string t_private_key, std::string t_public_key) : flight_safety_system::transport::fss_connection(), session(t_session_flags), ca_file(t_ca), private_key_file(t_private_key), public_key_file(t_public_key) {};
    fss_connection(int t_fd, unsigned int t_session_flags, std::string t_ca, std::string t_private_key, std::string t_public_key);
    fss_connection(fss_connection&) = delete;
    fss_connection(fss_connection&&) = delete;
    auto operator=(fss_connection&) -> fss_connection& = delete;
    auto operator=(fss_connection&&) -> fss_connection& = delete;
    virtual ~fss_connection();
    auto connectTo(const std::string &address, uint16_t port) -> bool override;
};

class fss_connection_client : public fss_connection {
public:
    fss_connection_client(std::string t_ca, std::string t_private_key, std::string t_public_key) : fss_connection(GNUTLS_CLIENT, std::move(t_ca), std::move(t_private_key), std::move(t_public_key)) {};
};


class fss_connection_server : public fss_connection {
public:
    fss_connection_server(int t_fd, std::string t_ca, std::string t_private_key, std::string t_public_key) : fss_connection(t_fd, GNUTLS_SERVER, std::move(t_ca), std::move(t_private_key), std::move(t_public_key)) {};
};

class fss_listen : public flight_safety_system::transport::fss_listen {
protected:
    std::string ca_file;
    std::string private_key_file;
    std::string public_key_file;
    auto newConnection(int fd) -> std::shared_ptr<flight_safety_system::transport::fss_connection> override;
public:
    fss_listen(uint16_t t_port, flight_safety_system::transport::fss_connect_cb t_cb, std::string t_ca, std::string t_private_key, std::string t_public_key) : flight_safety_system::transport::fss_listen(t_port, t_cb), ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)), public_key_file(std::move(t_public_key)) {};
};
}; // namespace transport_ssl
}; // namespace flight_safety_system