#include "fss-transport-ssl.hpp"
#include "transport.hpp"

#include <cstdint>
#include <gnutls/gnutls.h>
#include <gnutls/gnutlsxx.h>
#include <gnutls/x509.h>
#include <memory>
#include <ostream>
#include <sys/types.h>
#include <thread>

static void
recv_msg_thread(flight_safety_system::transport_ssl::fss_connection *conn)
{
    conn->processMessages();
}

flight_safety_system::transport_ssl::fss_connection::fss_connection(int t_fd, std::string t_ca, std::string t_private_key, std::string t_public_key) : ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)), public_key_file(std::move(t_public_key))
{
}

flight_safety_system::transport_ssl::fss_connection::~fss_connection()
{
}

flight_safety_system::transport_ssl::fss_connection_client::~fss_connection_client()
{
    if (this->usable)
    {
        try
        {
            this->session.bye(GNUTLS_SHUT_WR);
        }
        catch (gnutls::exception &ex)
        {
            std::cerr << "fss_connection shutdown, gnutls exception during bye" << std::endl;
        }
        this->usable = false;
    }
    this->disconnect();
}

flight_safety_system::transport_ssl::fss_connection_server::~fss_connection_server()
{
    if (this->usable)
    {
        try
        {
            this->session.bye(GNUTLS_SHUT_WR);
        }
        catch (gnutls::exception &ex)
        {
            std::cerr << "fss_connection shutdown, gnutls exception during bye" << std::endl;
        }
        this->usable = false;
    }
    this->disconnect();
}

flight_safety_system::transport_ssl::fss_connection_server::fss_connection_server(int t_fd, std::string t_ca, std::string t_private_key, std::string t_public_key) : flight_safety_system::transport_ssl::fss_connection(std::move(t_ca), std::move(t_private_key), std::move(t_public_key))
{
    this->setFd(t_fd);
    this->usable = this->setupSSL();
    if (this->usable)
    {
        this->startRecvThread(std::thread(recv_msg_thread, this));
    }
}

auto
flight_safety_system::transport_ssl::fss_connection_client::connectTo(const std::string &address, uint16_t port) -> bool
{
    this->hostname = address;
    struct sockaddr_storage remote = {};
    if (!convert_str_to_sa (address, port, &remote))
    {
        std::cerr << "Failed to convert '" << address << "' to a usable address\n";
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

    if (connect(this->getFd(), (struct sockaddr *)&remote, remote.ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)) < 0)
    {
        perror(("Failed to connect to " + address).c_str());
        return false;
    }

    this->usable = this->setupSSL();

    if (this->usable)
    {
        this->startRecvThread(std::thread(recv_msg_thread, this));
    }

    return this->usable;
}

void
flight_safety_system::transport_ssl::fss_connection::setupSession(gnutls::session &session)
{
    session.set_priority (nullptr, nullptr);

    this->credentials.set_x509_trust_file(this->ca_file.c_str(), GNUTLS_X509_FMT_PEM);
    this->credentials.set_x509_key_file(this->public_key_file.c_str(), this->private_key_file.c_str(), GNUTLS_X509_FMT_PEM);
    session.set_credentials(this->credentials);

    session.set_transport_ptr((gnutls_transport_ptr_t)(intptr_t)this->getFd());
}

auto
flight_safety_system::transport_ssl::fss_connection_client::setupSSL() -> bool
{
    this->setupSession(this->session);

    this->session.set_verify_cert(this->hostname.c_str(), 0);

    int ret = -2;
    try
    {
        ret = this->session.handshake();
    }
    catch (gnutls::exception &e)
    {
        std::cerr << "TLS error: " << e.what() << std::endl;
    }
    if (ret < 0)
    {
        std::cerr << "Failed to hand shake: " << ret << std::endl;
        return false;
    }

    return true;
}

auto
flight_safety_system::transport_ssl::fss_connection_server::setupSSL() -> bool
{
    this->setupSession(this->session);

    this->session.set_certificate_request(GNUTLS_CERT_REQUIRE);

    int ret = -1;
    try
    {
        ret = this->session.handshake();
    }
    catch(gnutls::exception &e)
    {
        std::cerr << "TLS error: " << e.what() << std::endl;
    }
    if (ret < 0)
    {
        std::cerr << "Failed to hand shake: " << ret << std::endl;
        return false;
    }

    std::vector<gnutls_datum_t> cert_list;

    if (this->session.get_peers_certificate(cert_list))
    {
        for (auto cert : cert_list)
        {
            gnutls_x509_crt_t cert_data;

            gnutls_x509_crt_init(&cert_data);

            gnutls_x509_crt_import(cert_data, &cert, GNUTLS_X509_FMT_DER);

            char name_buf[512];
            size_t dn_len = 512;
            gnutls_x509_crt_get_dn (cert_data, name_buf, &dn_len);
            std::string name = name_buf;
            /* Locate the CN=<name> section */
            std::size_t found = name.find("CN=");
            if (found != std::string::npos)
            {
                name.erase(0, found+3);
                /* Strip off any extra data (like ,O=...) */
                found = name.find(",");
                if (found != std::string::npos)
                {
                    name.erase(found);
                }
                this->possible_names.push_back(name);
            }
            gnutls_x509_crt_deinit(cert_data);
        }
    }

    return true;
}

auto
flight_safety_system::transport_ssl::fss_connection::sendSessionMsg(gnutls::session &session, const std::shared_ptr<flight_safety_system::transport::buf_len> &bl) -> bool
{
    if (!this->usable)
    {
        std::cerr << "Attempt to send on unusable transport_ssl::fss_connection" << std::endl;
        return false;
    }
    size_t to_send = bl->getLength();
    const char *data = bl->getData();
    try
    {
        session.send(data, to_send);
    }
    catch (gnutls::exception &ex)
    {
        std::cerr << "send: caught gnutls exception: " << ex.get_code() << ", " << ex.what() << std::endl;        
    }
    return true;
}

auto
flight_safety_system::transport_ssl::fss_connection_client::sendMsg(const std::shared_ptr<flight_safety_system::transport::buf_len> &bl) -> bool
{
    return this->sendSessionMsg(this->session, bl);
}

auto
flight_safety_system::transport_ssl::fss_connection_server::sendMsg(const std::shared_ptr<flight_safety_system::transport::buf_len> &bl) -> bool
{
    return this->sendSessionMsg(this->session, bl);
}

auto
flight_safety_system::transport_ssl::fss_connection::recvSessionBytes(gnutls::session &session, void *t_bytes, size_t t_max_bytes) -> ssize_t
{
    if (!this->usable)
    {
        std::cerr << "Attempt to recv on unusable transport_ssl::fss_connection" << std::endl;
        return -1;
    }
    ssize_t bytes_recved = -1;
    try
    {
        bytes_recved = session.recv(t_bytes, t_max_bytes);
    }
    catch (gnutls::exception &ex)
    {
        std::cerr << "recv: caught gnutls exception: " << ex.get_code() << ", " << ex.what() << std::endl;
    }
    return bytes_recved;
}

auto
flight_safety_system::transport_ssl::fss_connection_client::recvBytes(void *t_bytes, size_t t_max_bytes) -> ssize_t
{
    return this->recvSessionBytes(this->session, t_bytes, t_max_bytes);
}

auto
flight_safety_system::transport_ssl::fss_connection_server::recvBytes(void *t_bytes, size_t t_max_bytes) -> ssize_t
{
    return this->recvSessionBytes(this->session, t_bytes, t_max_bytes);
}
auto
flight_safety_system::transport_ssl::fss_listen::newConnection(int t_newfd) -> std::shared_ptr<flight_safety_system::transport::fss_connection>
{
    return std::make_shared<flight_safety_system::transport_ssl::fss_connection_server>(t_newfd, this->ca_file, this->private_key_file, this->public_key_file);
}

auto flight_safety_system::transport_ssl::fss_connection_server::getClientNames() -> std::list<std::string>
{
    return this->possible_names;
}