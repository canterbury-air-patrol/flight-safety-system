#include "fss-transport-ssl.hpp"
#include <gnutls/gnutls.h>
#include <gnutls/gnutlsxx.h>
#include <memory>
#include <sys/types.h>

using namespace flight_safety_system::transport_ssl;

fss_connection::fss_connection(int t_fd, unsigned int t_session_flags, std::string t_ca, std::string t_private_key, std::string t_public_key) : flight_safety_system::transport::fss_connection(t_fd), session(t_session_flags), ca_file(t_ca), private_key_file(t_public_key), public_key_file(t_public_key)
{
    this->usable = this->setupSSL();
}

fss_connection::~fss_connection()
{
    if (this->usable)
    {
        this->session.bye(GNUTLS_SHUT_RDWR);
    }
    this->usable = false;
}

auto
fss_connection::connectTo(const std::string &address, uint16_t port) -> bool
{
    if (flight_safety_system::transport::fss_connection::connectTo(address, port))
    {
        this->usable = this->setupSSL();
    }
    return this->usable;
}

auto
fss_connection::setupSSL() -> bool
{
    this->credentials.set_x509_trust_file(this->ca_file.c_str(), GNUTLS_X509_FMT_PEM);
    this->credentials.set_x509_key_file(this->public_key_file.c_str(), this->private_key_file.c_str(), GNUTLS_X509_FMT_PEM);
    this->session.set_credentials(this->credentials);
    this->session.set_transport_ptr((gnutls_transport_ptr_t) (ptrdiff_t) this->fd);

    int ret = this->session.handshake();
    if (ret < 0)
    {
        std::cerr << "Failed to hand shake: " << ret << std::endl;
        return false;
    }

    return true;
}

auto
fss_connection::sendMsg(const std::shared_ptr<flight_safety_system::transport::buf_len> &bl) -> bool
{
    if (!this->usable)
    {
        std::cerr << "Attempt to use unusable transport_ssl::fss_connection" << std::endl;
        return false;
    }
    size_t to_send = bl->getLength();
    const char *data = bl->getData();
    this->session.send(data, to_send);
    return true;
}

auto
fss_connection::recvBytes(void *t_bytes, size_t t_max_bytes) -> ssize_t
{
    if (!this->usable)
    {
        std::cerr << "Attempt to use unusable transport_ssl::fss_connection" << std::endl;
        return -1;
    }
    return this->session.recv(t_bytes, t_max_bytes);
}

auto
fss_listen::newConnection(int t_newfd) -> std::shared_ptr<flight_safety_system::transport::fss_connection>
{
    return std::make_shared<flight_safety_system::transport_ssl::fss_connection_server>(t_newfd, this->ca_file, this->private_key_file, this->public_key_file);
}