#include "fss-transport-ssl.hpp"
#include "fss-transport.hpp"
#include "fss-log.hpp"
#include "transport.hpp"

#include <array>
#include <cstdint>
#include <gnutls/gnutls.h>
#include <gnutls/gnutlsxx.h>
#include <gnutls/x509.h>
#include <memory>
#include <ostream>
#include <sys/types.h>
#include <thread>
#include <netinet/tcp.h>
#include <unistd.h>

#ifdef DEBUG
// This is defined in transport.cpp
extern const char *inet_ntop_stor(struct sockaddr_storage *src, char *dst, size_t dstlen, uint16_t *port);
#endif

namespace {

/* gnutls_init (via the gnutls::session constructors) can throw; contain it
 * so session construction follows the same no-throw contract as the
 * credential loaders. Returns the derived type so callers can use the
 * session-specific API without a downcast. */
template<typename SessionT> auto make_session_or_log(const char *what) -> std::unique_ptr<SessionT>
{
    try
    {
        return std::make_unique<SessionT>();
    }
    catch (const gnutls::exception &e)
    {
        FSS_LOG_ERROR("ssl", "Failed to initialise " << what << ": " << e.what());
        return nullptr;
    }
}

} // anonymous namespace

flight_safety_system::transport_ssl::fss_connection::fss_connection(std::string t_ca, std::string t_private_key,
                                                                    std::string t_public_key, std::string t_crl)
    : flight_safety_system::transport::fss_connection(), credentials(new gnutls::certificate_credentials()),
      ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)), public_key_file(std::move(t_public_key)),
      crl_file(std::move(t_crl))
{
}

flight_safety_system::transport_ssl::fss_connection::fss_connection(int t_fd, std::string t_ca,
                                                                    std::string t_private_key, std::string t_public_key,
                                                                    std::string t_crl)
    : flight_safety_system::transport::fss_connection(t_fd), credentials(new gnutls::certificate_credentials()),
      ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)), public_key_file(std::move(t_public_key)),
      crl_file(std::move(t_crl))
{
}

flight_safety_system::transport_ssl::fss_connection::~fss_connection()
{
    /* bye() cannot race the recv thread's session->recv(): both create()
     * paths capture the connection shared_ptr in the recv-thread lambda, so
     * this destructor only runs once that thread has dropped its reference —
     * i.e. after processMessages() returned (or on the recv thread itself,
     * where recv() has likewise finished). */
    if (this->usable.load())
    {
        try
        {
            this->session->bye(GNUTLS_SHUT_WR);
        }
        catch (gnutls::exception &ex)
        {
            FSS_LOG_WARN("ssl", "fss_connection shutdown, gnutls exception during bye");
        }
        this->usable.store(false);
    }
    this->disconnect();
}

flight_safety_system::transport_ssl::fss_connection_client::~fss_connection_client() = default;

flight_safety_system::transport_ssl::fss_connection_server::~fss_connection_server() = default;

flight_safety_system::transport_ssl::fss_connection_server::fss_connection_server(int t_fd, std::string t_ca,
                                                                                  std::string t_private_key,
                                                                                  std::string t_public_key,
                                                                                  std::string t_crl,
                                                                                  unsigned int t_handshake_timeout_ms)
    : flight_safety_system::transport_ssl::fss_connection(std::move(t_ca), std::move(t_private_key),
                                                          std::move(t_public_key), std::move(t_crl)),
      handshake_timeout_ms(t_handshake_timeout_ms)
{
    this->setFd(t_fd);
    this->usable.store(this->setupSSL());
}

auto flight_safety_system::transport_ssl::fss_connection_server::create(int t_fd, std::string t_ca,
                                                                        std::string t_private_key,
                                                                        std::string t_public_key, std::string t_crl,
                                                                        unsigned int t_handshake_timeout_ms)
    -> std::shared_ptr<fss_connection_server>
{
    auto conn = std::shared_ptr<fss_connection_server>(
        new fss_connection_server(t_fd, std::move(t_ca), std::move(t_private_key), std::move(t_public_key),
                                  std::move(t_crl), t_handshake_timeout_ms));
    if (!conn->usable.load())
    {
        /* Handshake failed or timed out: drop the dead connection (its
         * destructor closes the fd) rather than handing a zombie peer to the
         * accept callback. */
        return nullptr;
    }
    conn->startRecvThread(std::thread([conn]() -> void { conn->processMessages(); }));
    return conn;
}

flight_safety_system::transport_ssl::fss_connection_client::fss_connection_client(std::string t_ca,
                                                                                  std::string t_private_key,
                                                                                  std::string t_public_key)
    : fss_connection(std::move(t_ca), std::move(t_private_key), std::move(t_public_key))
{
}

auto flight_safety_system::transport_ssl::fss_connection_client::connectTo(const std::string &address, uint16_t port)
    -> bool
{
    this->hostname = address;
    struct sockaddr_storage remote = {};
    if (!convert_str_to_sa(address, port, &remote))
    {
        FSS_LOG_ERROR("ssl", "Failed to convert '" << address << "' to a usable address");
        return false;
    }

#ifdef DEBUG
    char addr_str[INET6_ADDRSTRLEN];
    uint16_t client_port;
    inet_ntop_stor(&remote, addr_str, INET6_ADDRSTRLEN, &client_port);
    std::cout << "Trying to connect to " << address << " (" << addr_str << "):" << port << std::endl;
#endif

    if (this->getFd() == -1)
    {
        int new_fd = socket(remote.ss_family == AF_INET ? PF_INET : PF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (new_fd < 0)
        {
            FSS_PERROR("ssl", "Failed to create socket");
            return false;
        }
        this->setFd(new_fd);
    }

    // Limit the total number of SYN's that are sent
    int synRetries = 2;
    if (setsockopt(this->getFd(), IPPROTO_TCP, TCP_SYNCNT, &synRetries, sizeof(synRetries)) < 0)
    {
        FSS_PERROR("ssl", "setsockopt TCP_SYNCNT failed, using kernel default");
    }

    if (connect(this->getFd(), as_sockaddr(&remote),
                remote.ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)) < 0)
    {
        FSS_PERROR("ssl", "Failed to connect to " + address);
        safe_close_fd(this->getFd(), "ssl/connect");
        this->setFd(-1);
        return false;
    }

    set_tcp_keepalive(this->getFd());

    this->usable.store(this->setupSSL());

    return this->usable.load();
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
auto flight_safety_system::transport_ssl::fss_connection_client::create(std::string t_ca, std::string t_private_key,
                                                                        std::string t_public_key,
                                                                        const std::string &address, uint16_t port)
    -> std::shared_ptr<fss_connection_client>
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    auto conn =
        std::make_shared<fss_connection_client>(std::move(t_ca), std::move(t_private_key), std::move(t_public_key));
    if (!conn->connectTo(address, port))
    {
        return nullptr;
    }
    conn->startRecvThread(std::thread([conn]() -> void { conn->processMessages(); }));
    return conn;
}

/* The gnutlsxx wrappers throw on failure (missing file, unreadable key,
 * malformed PEM). Each loader below contains the exception for one step so
 * misconfiguration surfaces as setupSession() returning false — never as a
 * gnutls::exception escaping the library API (see the contract note in the
 * header). */
auto flight_safety_system::transport_ssl::fss_connection::loadTrustFile() -> bool
{
    try
    {
        this->credentials->set_x509_trust_file(this->ca_file.c_str(), GNUTLS_X509_FMT_PEM);
        return true;
    }
    catch (const gnutls::exception &e)
    {
        FSS_LOG_ERROR("ssl", "Failed to load CA trust file '" << this->ca_file << "': " << e.what());
        return false;
    }
}

auto flight_safety_system::transport_ssl::fss_connection::loadKeyPair() -> bool
{
    try
    {
        this->credentials->set_x509_key_file(this->public_key_file.c_str(), this->private_key_file.c_str(),
                                             GNUTLS_X509_FMT_PEM);
        return true;
    }
    catch (const gnutls::exception &e)
    {
        FSS_LOG_ERROR("ssl", "Failed to load key pair (cert '" << this->public_key_file << "', key '"
                                                               << this->private_key_file << "'): " << e.what());
        return false;
    }
}

auto flight_safety_system::transport_ssl::fss_connection::loadCrl() -> bool
{
    if (this->crl_file.empty())
    {
        return true;
    }
    try
    {
        this->credentials->set_x509_crl_file(this->crl_file.c_str(), GNUTLS_X509_FMT_PEM);
        return true;
    }
    catch (const gnutls::exception &e)
    {
        FSS_LOG_ERROR("ssl", "Failed to load CRL file '" << this->crl_file << "': " << e.what());
        return false;
    }
}

auto flight_safety_system::transport_ssl::fss_connection::attachCredentials() -> bool
{
    try
    {
        this->session->set_credentials(*this->credentials);
        return true;
    }
    catch (const gnutls::exception &e)
    {
        FSS_LOG_ERROR("ssl", "Failed to attach credentials to TLS session: " << e.what());
        return false;
    }
}

auto flight_safety_system::transport_ssl::fss_connection::setupSession() -> bool
{
    const char *err_pos = nullptr;
    try
    {
        this->session->set_priority("SECURE256:+SECURE128:-VERS-TLS1.0:-VERS-TLS1.1:%SERVER_PRECEDENCE", &err_pos);
    }
    catch (const gnutls::exception &ex)
    {
        FSS_LOG_ERROR("ssl",
                      "Failed to set TLS priority: " << ex.what() << (err_pos ? std::string(" near: ") + err_pos : ""));
        this->usable.store(false);
        return false;
    }

    if (!this->loadTrustFile() || !this->loadKeyPair() || !this->loadCrl() || !this->attachCredentials())
    {
        return false;
    }

    gnutls_transport_set_int(this->session->ptr(), this->getFd());
    return true;
}

auto flight_safety_system::transport_ssl::fss_connection_client::setupSSL() -> bool
{
    auto new_session = make_session_or_log<gnutls::client_session>("TLS client session");
    if (new_session == nullptr)
    {
        return false;
    }
    auto *clientSession = new_session.get();
    this->session = std::move(new_session);

    if (!this->setupSession())
    {
        return false;
    }

    clientSession->set_verify_cert(this->hostname.c_str(), 0);

    /* Bound the handshake so a server that accepts the TCP connection but
     * stalls the TLS handshake cannot hang connectTo() indefinitely. */
    gnutls_handshake_set_timeout(this->session->ptr(), default_handshake_timeout_ms);

    int ret = -2;
    try
    {
        ret = this->session->handshake();
    }
    catch (gnutls::exception &e)
    {
        FSS_LOG_ERROR("ssl", "TLS error: " << e.what());
    }
    if (ret < 0)
    {
        FSS_LOG_ERROR("ssl", "Failed to hand shake: " << ret);
        return false;
    }

    return true;
}

auto flight_safety_system::transport_ssl::fss_connection_server::setupSSL() -> bool
{
    auto new_session = make_session_or_log<gnutls::server_session>("TLS server session");
    if (new_session == nullptr)
    {
        return false;
    }
    auto *serverSession = new_session.get();
    this->session = std::move(new_session);

    if (!this->setupSession())
    {
        return false;
    }

    serverSession->set_certificate_request(GNUTLS_CERT_REQUIRE);

    /* GNUTLS_CERT_REQUIRE only forces the client to PRESENT a certificate;
     * on its own it does not check that the certificate chains to our
     * trusted CA. Without this, a self-signed cert carrying a known asset
     * CN would pass the handshake and then satisfy the CN-based identity
     * check — an authentication bypass. Enable inline verification so the
     * handshake itself fails for any client cert that does not validate
     * against the trust file (the client side already does the equivalent
     * via set_verify_cert). NULL hostname: a client certificate's identity
     * is its CN, matched at the application layer, not a hostname. */
    gnutls_session_set_verify_cert(this->session->ptr(), nullptr, 0);

    /* Bound the handshake so a peer that completes the TCP connection but then
     * stalls (sends no/partial ClientHello) cannot occupy this setup worker
     * indefinitely. With the default int transport (gnutls_transport_set_int
     * in setupSession), gnutls drives the handshake against its system
     * pull-timeout function and returns GNUTLS_E_TIMEDOUT at the deadline. */
    gnutls_handshake_set_timeout(this->session->ptr(), this->handshake_timeout_ms);

    int ret = -1;
    try
    {
        ret = this->session->handshake();
    }
    catch (gnutls::exception &e)
    {
        FSS_LOG_ERROR("ssl", "TLS error: " << e.what());
    }
    if (ret < 0)
    {
        FSS_LOG_ERROR("ssl", "Failed to hand shake: " << ret);
        return false;
    }

    std::vector<gnutls_datum_t> cert_list;

    /* Only the leaf cert (index 0) carries the peer's identity; CNs of
     * intermediate CAs in the chain are issuer names, not identities, and
     * must not be accepted as a valid client name. */
    if (this->session->get_peers_certificate(cert_list) && !cert_list.empty())
    {
        gnutls_x509_crt_t cert_data = {};

        int rc = gnutls_x509_crt_init(&cert_data);
        if (rc < 0)
        {
            FSS_LOG_ERROR("ssl", "Failed to initialize X.509 certificate: " << gnutls_strerror(rc));
            return false;
        }

        rc = gnutls_x509_crt_import(cert_data, &cert_list[0], GNUTLS_X509_FMT_DER);
        if (rc < 0)
        {
            FSS_LOG_ERROR("ssl", "Failed to import X.509 certificate: " << gnutls_strerror(rc));
            gnutls_x509_crt_deinit(cert_data);
            return false;
        }

        constexpr size_t dn_max_len = 512;
        std::array<char, dn_max_len> name_buf{};
        size_t name_len = dn_max_len;
        rc = gnutls_x509_crt_get_dn_by_oid(cert_data, GNUTLS_OID_X520_COMMON_NAME, 0, 0, name_buf.data(), &name_len);
        if (rc == GNUTLS_E_SUCCESS && name_len > 0)
        {
            this->possible_names.emplace_back(name_buf.data(), name_len);
        }
        gnutls_x509_crt_deinit(cert_data);
    }

    return true;
}

auto flight_safety_system::transport_ssl::fss_connection::sendMsg(
    const std::shared_ptr<flight_safety_system::transport::buf_len> &bl) -> bool
{
    if (!this->usable.load())
    {
        FSS_LOG_ERROR("ssl", "Attempt to send on unusable transport_ssl::fss_connection");
        return false;
    }
    size_t to_send = bl->getLength();
    const char *data = bl->getData();
    size_t sent = 0;
    while (sent < to_send)
    {
        ssize_t transferred = 0;
        try
        {
            transferred = this->session->send(&data[sent], to_send - sent);
        }
        catch (gnutls::exception &ex)
        {
            FSS_LOG_ERROR("ssl", "send: caught gnutls exception: " << ex.get_code() << ", " << ex.what());
            this->usable.store(false);
            return false;
        }
        if (transferred <= 0)
        {
            this->usable.store(false);
            return false;
        }
        sent += transferred;
    }
    return true;
}

auto flight_safety_system::transport_ssl::fss_connection::recvBytes(void *t_bytes, size_t t_max_bytes) -> ssize_t
{
    if (!this->usable.load())
    {
        FSS_LOG_ERROR("ssl", "Attempt to recv on unusable transport_ssl::fss_connection");
        return -2;
    }
    for (;;)
    {
        ssize_t bytes_recved = -1;
        try
        {
            bytes_recved = this->session->recv(t_bytes, t_max_bytes);
        }
        catch (gnutls::exception &ex)
        {
            if (ex.get_code() == GNUTLS_E_AGAIN || ex.get_code() == GNUTLS_E_INTERRUPTED)
            {
                continue;
            }
            FSS_LOG_ERROR("ssl", "recv: caught gnutls exception: " << ex.get_code() << ", " << ex.what());
            this->usable.store(false);
            return bytes_recved;
        }
        /* Some C++ wrapper variants return AGAIN/INTERRUPTED rather than throwing. */
        if (bytes_recved == GNUTLS_E_AGAIN || bytes_recved == GNUTLS_E_INTERRUPTED)
        {
            continue;
        }
        return bytes_recved;
    }
}

auto flight_safety_system::transport_ssl::fss_listen::newConnection(int t_newfd)
    -> std::shared_ptr<flight_safety_system::transport::fss_connection>
{
    return flight_safety_system::transport_ssl::fss_connection_server::create(
        t_newfd, this->ca_file, this->private_key_file, this->public_key_file, this->crl_file,
        this->handshake_timeout_ms);
}

flight_safety_system::transport_ssl::fss_listen::fss_listen(uint16_t t_port,
                                                            flight_safety_system::transport::fss_connect_cb t_cb,
                                                            std::string t_ca, std::string t_private_key,
                                                            std::string t_public_key, std::string t_crl,
                                                            unsigned int t_handshake_timeout_ms,
                                                            size_t t_max_concurrent_handshakes)
    : flight_safety_system::transport::fss_listen(t_port, std::move(t_cb), defer_start_t{}), ca_file(std::move(t_ca)),
      private_key_file(std::move(t_private_key)), public_key_file(std::move(t_public_key)), crl_file(std::move(t_crl)),
      handshake_timeout_ms(t_handshake_timeout_ms)
{
    /* Apply the concurrency bound (0 = keep the base default) before
     * startListening() so the accept thread reads a settled value. */
    if (t_max_concurrent_handshakes > 0)
    {
        this->setMaxConcurrentSetups(t_max_concurrent_handshakes);
    }
    /* Start the accept thread only now that this object is fully
     * constructed: the thread virtual-dispatches into newConnection(),
     * which reads the path strings initialised above. Starting it from the
     * base constructor (the default behaviour) would race that. */
    this->startListening();
}

flight_safety_system::transport_ssl::fss_listen::~fss_listen()
{
    /* Join the accept thread while the cert/key path strings it reads in
     * newConnection() are still alive; the base destructor's disconnect()
     * would run only after they are destroyed. disconnect() is idempotent,
     * so the base's call becomes a no-op. */
    this->disconnect();
}

auto flight_safety_system::transport_ssl::fss_connection_server::getClientNames() -> std::list<std::string>
{
    return this->possible_names;
}

auto flight_safety_system::transport_ssl::fss_connection_server::isPeerCertRevoked(const std::string &t_crl_file) const
    -> bool
{
    if (t_crl_file.empty() || !this->session)
    {
        return false;
    }

    std::vector<gnutls_datum_t> cert_list;
    if (!this->session->get_peers_certificate(cert_list) || cert_list.empty())
    {
        return false;
    }

    struct Guard {
        gnutls_x509_crt_t cert = nullptr;
        gnutls_x509_crl_t crl = nullptr;
        ~Guard()
        {
            if (crl != nullptr)
            {
                gnutls_x509_crl_deinit(crl);
            }
            if (cert != nullptr)
            {
                gnutls_x509_crt_deinit(cert);
            }
        }
    } guard;

    int ret = gnutls_x509_crt_init(&guard.cert);
    if (ret < 0)
    {
        FSS_LOG_ERROR("ssl", "Failed to initialise x509 certificate object: " << gnutls_strerror(ret));
        return false;
    }
    if (gnutls_x509_crt_import(guard.cert, &cert_list[0], GNUTLS_X509_FMT_DER) < 0)
    {
        return false;
    }

    gnutls_datum_t crl_data = {};
    if (gnutls_load_file(t_crl_file.c_str(), &crl_data) < 0)
    {
        FSS_LOG_ERROR("ssl", "Failed to load CRL file: " << t_crl_file);
        return false;
    }

    ret = gnutls_x509_crl_init(&guard.crl);
    if (ret < 0)
    {
        FSS_LOG_ERROR("ssl", "Failed to initialise x509 CRL object: " << gnutls_strerror(ret));
        gnutls_free(crl_data.data);
        return false;
    }
    int rc = gnutls_x509_crl_import(guard.crl, &crl_data, GNUTLS_X509_FMT_PEM);
    gnutls_free(crl_data.data);
    if (rc < 0)
    {
        FSS_LOG_ERROR("ssl", "Failed to parse CRL file: " << t_crl_file << ": " << gnutls_strerror(rc));
        return false;
    }

    int revoked = gnutls_x509_crt_check_revocation(guard.cert, &guard.crl, 1);
    return revoked > 0;
}

auto flight_safety_system::transport_ssl::fss_connection::getSessionDesc() -> std::string
{
    if (!this->session)
    {
        return {};
    }
    char *desc = gnutls_session_get_desc(this->session->ptr());
    if (desc == nullptr)
    {
        return {};
    }
    std::string result(desc);
    gnutls_free(desc);
    return result;
}
