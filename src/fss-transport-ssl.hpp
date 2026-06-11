#include <fss-transport.hpp>

#include <atomic>
#include <list>
#include <string>

namespace gnutls {
class certificate_credentials;
class session;
} // namespace gnutls

namespace flight_safety_system {
namespace transport_ssl {
/* Exception contract: no gnutls::exception escapes this library's public
 * entry points. Misconfiguration (missing/unreadable CA, key, cert, or CRL
 * files) and TLS setup failures surface as fss_connection_client::create()
 * returning nullptr, connectTo() returning false, or — on the server accept
 * path — a connection delivered to the connect callback in an unusable
 * state (recv thread never started, getMsg() stays null). The cause is
 * logged. Callers must not need a try/catch around connection setup. */
class fss_connection : public flight_safety_system::transport::fss_connection {
private:
    std::unique_ptr<gnutls::certificate_credentials> credentials;
    std::string ca_file;
    std::string private_key_file;
    std::string public_key_file;
    std::string crl_file;
    /* Each contains the gnutlsxx exception for one credential-loading step,
     * logging the offending file and returning false (see the exception
     * contract above). */
    auto loadTrustFile() -> bool;
    auto loadKeyPair() -> bool;
    auto loadCrl() -> bool;
    auto attachCredentials() -> bool;
protected:
    /* session and possible_names (in fss_connection_server) are written only
     * during setupSSL(), which completes before startRecvThread() — so they
     * are immutable once any second thread exists and need no locking.
     * Concurrent session->send() (sender threads, serialised by the base
     * class send_lock) and session->recv() (recv thread) on one session is
     * permitted by GnuTLS for TLS without rehandshake; this design relies on
     * that. */
    std::unique_ptr<gnutls::session> session{nullptr};
    /* Written by the recv thread (recvBytes failure), sender threads (send
     * failure), and the destructor; read by all of them — must be atomic.
     * Advisory only: a send that races a concurrent clear fails inside
     * gnutls and clears the flag again. */
    std::atomic<bool> usable{false};
    auto sendMsg(const std::shared_ptr<flight_safety_system::transport::buf_len>& bl) -> bool override;
    auto recvBytes(void* bytes, size_t max_bytes) -> ssize_t override;
    auto setupSession() -> bool;
public:
    fss_connection(std::string t_ca, std::string t_private_key, std::string t_public_key, std::string t_crl = {});
    fss_connection(int t_fd, std::string t_ca, std::string t_private_key, std::string t_public_key,
                   std::string t_crl = {});
    fss_connection(fss_connection&) = delete;
    fss_connection(fss_connection&&) = delete;
    ~fss_connection() override;
    auto getSessionDesc() -> std::string;
};

class fss_connection_client : public fss_connection {
private:
    std::string hostname{};
protected:
    auto setupSSL() -> bool;
public:
    fss_connection_client(std::string t_ca, std::string t_private_key, std::string t_public_key);
    fss_connection_client(fss_connection_client&) = delete;
    fss_connection_client(fss_connection_client&&) = delete;
    auto operator=(fss_connection_client&) -> fss_connection& = delete;
    auto operator=(fss_connection_client&&) -> fss_connection& = delete;
    ~fss_connection_client() override;
    auto connectTo(const std::string& address, uint16_t port) -> bool override;
    static auto create(std::string t_ca, std::string t_private_key, std::string t_public_key,
                       const std::string& address, uint16_t port) -> std::shared_ptr<fss_connection_client>;
};


class fss_connection_server : public fss_connection {
private:
    std::list<std::string> possible_names{};
    fss_connection_server(int t_fd, std::string t_ca, std::string t_private_key, std::string t_public_key,
                          std::string t_crl = {});
protected:
    auto setupSSL() -> bool;
public:
    fss_connection_server(fss_connection_server&) = delete;
    fss_connection_server(fss_connection_server&&) = delete;
    auto operator=(fss_connection_server&) -> fss_connection_server& = delete;
    auto operator=(fss_connection_server&&) -> fss_connection_server& = delete;
    ~fss_connection_server() override;
    static auto create(int t_fd, std::string t_ca, std::string t_private_key, std::string t_public_key,
                       std::string t_crl) -> std::shared_ptr<fss_connection_server>;
    auto getClientNames() -> std::list<std::string> override;
    auto isPeerCertRevoked(const std::string& t_crl_file) const -> bool override;
};

class fss_listen : public flight_safety_system::transport::fss_listen {
private:
    std::string ca_file;
    std::string private_key_file;
    std::string public_key_file;
    std::string crl_file;
protected:
    auto newConnection(int fd) -> std::shared_ptr<flight_safety_system::transport::fss_connection> override;
public:
    fss_listen(uint16_t t_port, flight_safety_system::transport::fss_connect_cb t_cb, std::string t_ca,
               std::string t_private_key, std::string t_public_key, std::string t_crl = {});
    /* Copy/move are already deleted via the base class. */
    /* Joins the accept thread before this class's members are destroyed:
     * that thread reads the cert/key path strings above in newConnection(),
     * and the base destructor's join runs only after derived members are
     * already gone — a use-after-free window without this. */
    ~fss_listen() override;
};
} // namespace transport_ssl
} // namespace flight_safety_system
