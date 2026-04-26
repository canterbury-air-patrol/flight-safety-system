#include "fss-transport-ssl.hpp"
#include "fss-transport.hpp"
#include "fss-log.hpp"
#include "transport.hpp"

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
extern const char *
inet_ntop_stor(struct sockaddr_storage *src, char *dst, size_t dstlen, uint16_t *port);
#endif

flight_safety_system::transport_ssl::fss_connection::fss_connection(std::string t_ca, std::string t_private_key, std::string t_public_key, std::string t_crl) : flight_safety_system::transport::fss_connection(), credentials(new gnutls::certificate_credentials()), ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)), public_key_file(std::move(t_public_key)), crl_file(std::move(t_crl))
{
}

flight_safety_system::transport_ssl::fss_connection::fss_connection(int t_fd, std::string t_ca, std::string t_private_key, std::string t_public_key, std::string t_crl) : flight_safety_system::transport::fss_connection(t_fd), credentials(new gnutls::certificate_credentials()), ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)), public_key_file(std::move(t_public_key)), crl_file(std::move(t_crl))
{
}

flight_safety_system::transport_ssl::fss_connection::~fss_connection()
{
    if (this->usable)
    {
        try
        {
            this->session->bye(GNUTLS_SHUT_WR);
        }
        catch (gnutls::exception &ex)
        {
            FSS_LOG_WARN("ssl", "fss_connection shutdown, gnutls exception during bye");
        }
        this->usable = false;
    }
    this->disconnect();
}

flight_safety_system::transport_ssl::fss_connection_client::~fss_connection_client() = default;

flight_safety_system::transport_ssl::fss_connection_server::~fss_connection_server() = default;

flight_safety_system::transport_ssl::fss_connection_server::fss_connection_server(int t_fd, std::string t_ca, std::string t_private_key, std::string t_public_key, std::string t_crl) : flight_safety_system::transport_ssl::fss_connection(std::move(t_ca), std::move(t_private_key), std::move(t_public_key), std::move(t_crl))
{
    this->setFd(t_fd);
    this->usable = this->setupSSL();
}

auto
flight_safety_system::transport_ssl::fss_connection_server::create(int t_fd, std::string t_ca, std::string t_private_key, std::string t_public_key, std::string t_crl) -> std::shared_ptr<fss_connection_server>
{
    auto conn = std::shared_ptr<fss_connection_server>(new fss_connection_server(t_fd, std::move(t_ca), std::move(t_private_key), std::move(t_public_key), std::move(t_crl)));
    if (conn->usable)
    {
        conn->startRecvThread(std::thread([conn]() -> void { conn->processMessages(); }));
    }
    return conn;
}

flight_safety_system::transport_ssl::fss_connection_client::fss_connection_client(std::string t_ca, std::string t_private_key, std::string t_public_key) : fss_connection(std::move(t_ca), std::move(t_private_key), std::move(t_public_key))
{
}

auto
flight_safety_system::transport_ssl::fss_connection_client::connectTo(const std::string &address, uint16_t port) -> bool
{
    this->hostname = address;
    struct sockaddr_storage remote = {};
    if (!convert_str_to_sa (address, port, &remote))
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
        this->setFd(socket(remote.ss_family == AF_INET ? PF_INET : PF_INET6, SOCK_STREAM, IPPROTO_TCP));
    }

    // Limit the total number of SYN's that are sent
    int synRetries = 2;
    setsockopt(this->getFd(), IPPROTO_TCP, TCP_SYNCNT, &synRetries, sizeof(synRetries));

    if (connect(this->getFd(), reinterpret_cast<struct sockaddr *>(&remote), remote.ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)) < 0)
    {
        FSS_PERROR("ssl", "Failed to connect to " + address);
        close(this->getFd());
        this->setFd(-1);
        return false;
    }

    set_tcp_keepalive(this->getFd());

    this->usable = this->setupSSL();

    return this->usable;
}

auto
flight_safety_system::transport_ssl::fss_connection_client::create(std::string t_ca, std::string t_private_key, std::string t_public_key, const std::string &address, uint16_t port) -> std::shared_ptr<fss_connection_client>
{
    auto conn = std::make_shared<fss_connection_client>(std::move(t_ca), std::move(t_private_key), std::move(t_public_key));
    if (!conn->connectTo(address, port))
    {
        return nullptr;
    }
    conn->startRecvThread(std::thread([conn]() -> void { conn->processMessages(); }));
    return conn;
}

auto
flight_safety_system::transport_ssl::fss_connection::setupSession() -> bool
{
    const char *err_pos = nullptr;
    try
    {
        this->session->set_priority(
            "SECURE256:+SECURE128:-VERS-TLS1.0:-VERS-TLS1.1:%SERVER_PRECEDENCE",
            &err_pos);
    }
    catch (gnutls::exception &ex)
    {
        FSS_LOG_ERROR("ssl", "Failed to set TLS priority: " << ex.what()
                      << (err_pos ? std::string(" near: ") + err_pos : ""));
        this->usable = false;
        return false;
    }

    this->credentials->set_x509_trust_file(this->ca_file.c_str(), GNUTLS_X509_FMT_PEM);
    this->credentials->set_x509_key_file(this->public_key_file.c_str(), this->private_key_file.c_str(), GNUTLS_X509_FMT_PEM);
    if (!this->crl_file.empty())
    {
        try
        {
            this->credentials->set_x509_crl_file(this->crl_file.c_str(), GNUTLS_X509_FMT_PEM);
        }
        catch (gnutls::exception &e)
        {
            FSS_LOG_ERROR("ssl", "Failed to load CRL file '" << this->crl_file << "': " << e.what());
            return false;
        }
    }
    this->session->set_credentials(*this->credentials);

    this->session->set_transport_ptr((gnutls_transport_ptr_t)(intptr_t)this->getFd());
    return true;
}

auto
flight_safety_system::transport_ssl::fss_connection_client::setupSSL() -> bool
{
    auto clientSession = new gnutls::client_session();

    this->session = std::unique_ptr<gnutls::session>(clientSession);

    if (!this->setupSession())
    {
        return false;
    }

    clientSession->set_verify_cert(this->hostname.c_str(), 0);

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

auto
flight_safety_system::transport_ssl::fss_connection_server::setupSSL() -> bool
{
    auto serverSession = new gnutls::server_session();

    this->session = std::unique_ptr<gnutls::session>(serverSession);

    if (!this->setupSession())
    {
        return false;
    }

    serverSession->set_certificate_request(GNUTLS_CERT_REQUIRE);

    int ret = -1;
    try
    {
        ret = this->session->handshake();
    }
    catch(gnutls::exception &e)
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

        const size_t dn_max_len = 512;
        char name_buf[dn_max_len];
        size_t name_len = dn_max_len;
        rc = gnutls_x509_crt_get_dn_by_oid(cert_data, GNUTLS_OID_X520_COMMON_NAME,
                                                 0, 0, name_buf, &name_len);
        if (rc == GNUTLS_E_SUCCESS && name_len > 0)
        {
            this->possible_names.push_back(std::string(name_buf, name_len));
        }
        gnutls_x509_crt_deinit(cert_data);
    }

    return true;
}

auto
flight_safety_system::transport_ssl::fss_connection::sendMsg(const std::shared_ptr<flight_safety_system::transport::buf_len> &bl) -> bool
{
    if (!this->usable)
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
            this->usable = false;
            return false;
        }
        if (transferred <= 0)
        {
            this->usable = false;
            return false;
        }
        sent += transferred;
    }
    return true;
}

auto
flight_safety_system::transport_ssl::fss_connection::recvBytes(void *t_bytes, size_t t_max_bytes) -> ssize_t
{
    if (!this->usable)
    {
        FSS_LOG_ERROR("ssl", "Attempt to recv on unusable transport_ssl::fss_connection");
        return -2;
    }
    ssize_t bytes_recved = -1;
    try
    {
        bytes_recved = this->session->recv(t_bytes, t_max_bytes);
    }
    catch (gnutls::exception &ex)
    {
        FSS_LOG_ERROR("ssl", "recv: caught gnutls exception: " << ex.get_code() << ", " << ex.what());
        if (ex.get_code() != GNUTLS_E_AGAIN)
        {
            this->usable = false;
        }
    }
    return bytes_recved;
}

auto
flight_safety_system::transport_ssl::fss_listen::newConnection(int t_newfd) -> std::shared_ptr<flight_safety_system::transport::fss_connection>
{
    return flight_safety_system::transport_ssl::fss_connection_server::create(t_newfd, this->ca_file, this->private_key_file, this->public_key_file, this->crl_file);
}

flight_safety_system::transport_ssl::fss_listen::fss_listen(uint16_t t_port, flight_safety_system::transport::fss_connect_cb t_cb, std::string t_ca, std::string t_private_key, std::string t_public_key, std::string t_crl) : flight_safety_system::transport::fss_listen(t_port, t_cb), ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)), public_key_file(std::move(t_public_key)), crl_file(std::move(t_crl))
{
}

auto flight_safety_system::transport_ssl::fss_connection_server::getClientNames() -> std::list<std::string>
{
    return this->possible_names;
}

auto flight_safety_system::transport_ssl::fss_connection_server::isPeerCertRevoked(const std::string &t_crl_file) const -> bool
{
    if (t_crl_file.empty() || !this->session) { return false; }

    std::vector<gnutls_datum_t> cert_list;
    if (!this->session->get_peers_certificate(cert_list) || cert_list.empty()) { return false; }

    gnutls_x509_crt_t cert = nullptr;
    gnutls_x509_crt_init(&cert);
    if (gnutls_x509_crt_import(cert, &cert_list[0], GNUTLS_X509_FMT_DER) < 0)
    {
        gnutls_x509_crt_deinit(cert);
        return false;
    }

    gnutls_datum_t crl_data = {};
    if (gnutls_load_file(t_crl_file.c_str(), &crl_data) < 0)
    {
        FSS_LOG_ERROR("ssl", "Failed to load CRL file: " << t_crl_file);
        gnutls_x509_crt_deinit(cert);
        return false;
    }

    gnutls_x509_crl_t crl = nullptr;
    gnutls_x509_crl_init(&crl);
    int rc = gnutls_x509_crl_import(crl, &crl_data, GNUTLS_X509_FMT_PEM);
    gnutls_free(crl_data.data);
    if (rc < 0)
    {
        FSS_LOG_ERROR("ssl", "Failed to parse CRL file: " << t_crl_file << ": " << gnutls_strerror(rc));
        gnutls_x509_crl_deinit(crl);
        gnutls_x509_crt_deinit(cert);
        return false;
    }

    int revoked = gnutls_x509_crt_check_revocation(cert, &crl, 1);
    gnutls_x509_crl_deinit(crl);
    gnutls_x509_crt_deinit(cert);
    return revoked > 0;
}

auto flight_safety_system::transport_ssl::fss_connection::getSessionDesc() -> std::string
{
    if (!this->session) { return {}; }
    char *desc = gnutls_session_get_desc(this->session->ptr());
    if (desc == nullptr) { return {}; }
    std::string result(desc);
    gnutls_free(desc);
    return result;
}
