#include "fss-transport-ssl.hpp"
#include "transport.hpp"

#include <cstdint>
#include <gnutls/gnutls.h>
#include <gnutls/gnutlsxx.h>
#include <memory>
#include <ostream>
#include <sys/types.h>
#include <thread>

static void
recv_msg_thread(flight_safety_system::transport_ssl::fss_connection *conn)
{
    std::cerr << "Recv thread running" << std::endl;
    conn->processMessages();
    std::cerr << "Recv thread ended" << std::endl;
}

flight_safety_system::transport_ssl::fss_connection::fss_connection(int t_fd, unsigned int t_session_flags, std::string t_ca, std::string t_private_key, std::string t_public_key) : flight_safety_system::transport::fss_connection(), session(t_session_flags), ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)), public_key_file(std::move(t_public_key))
{
    this->fd = t_fd;
    this->usable = this->setupSSL();
    if (this->usable)
    {
        this->recv_thread = std::thread(recv_msg_thread, this);
    }
}

flight_safety_system::transport_ssl::fss_connection::~fss_connection()
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

auto
flight_safety_system::transport_ssl::fss_connection::connectTo(const std::string &address, uint16_t port) -> bool
{

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

    if (this->fd == -1)
    {
        this->fd = socket(remote.ss_family == AF_INET ? PF_INET : PF_INET6, SOCK_STREAM, IPPROTO_TCP);
    }

    if (connect(this->fd, (struct sockaddr *)&remote, remote.ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)) < 0)
    {
        perror(("Failed to connect to " + address).c_str());
        return false;
    }

    this->usable = this->setupSSL();

    if (this->usable)
    {
        this->recv_thread = std::thread(recv_msg_thread, this);
    }

    return this->usable;
}

auto
flight_safety_system::transport_ssl::fss_connection::setupSSL() -> bool
{
    this->session.set_priority (nullptr, nullptr);

    this->credentials.set_x509_trust_file(this->ca_file.c_str(), GNUTLS_X509_FMT_PEM);
    this->credentials.set_x509_key_file(this->public_key_file.c_str(), this->private_key_file.c_str(), GNUTLS_X509_FMT_PEM);
    this->session.set_credentials(this->credentials);

    this->session.set_transport_ptr((gnutls_transport_ptr_t)(intptr_t)this->fd);

    int ret = this->session.handshake();
    if (ret < 0)
    {
        std::cerr << "Failed to hand shake: " << ret << std::endl;
        return false;
    }

    return true;
}

auto
flight_safety_system::transport_ssl::fss_connection::sendMsg(const std::shared_ptr<flight_safety_system::transport::buf_len> &bl) -> bool
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
        this->session.send(data, to_send);
    }
    catch (gnutls::exception &ex)
    {
        std::cerr << "send: caught gnutls exception: " << ex.get_code() << ", " << ex.what() << std::endl;        
    }
    return true;
}

auto
flight_safety_system::transport_ssl::fss_connection::recvBytes(void *t_bytes, size_t t_max_bytes) -> ssize_t
{
    if (!this->usable)
    {
        std::cerr << "Attempt to recv on unusable transport_ssl::fss_connection" << std::endl;
        return -1;
    }
    ssize_t bytes_recved = -1;
    try
    {
        bytes_recved = this->session.recv(t_bytes, t_max_bytes);
    }
    catch (gnutls::exception &ex)
    {
        std::cerr << "recv: caught gnutls exception: " << ex.get_code() << ", " << ex.what() << std::endl;
    }
    return bytes_recved;
}

auto
flight_safety_system::transport_ssl::fss_listen::newConnection(int t_newfd) -> std::shared_ptr<flight_safety_system::transport::fss_connection>
{
    return std::make_shared<flight_safety_system::transport_ssl::fss_connection_server>(t_newfd, this->ca_file, this->private_key_file, this->public_key_file);
}