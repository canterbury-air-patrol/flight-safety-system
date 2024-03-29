#include <bits/stdint-uintn.h>
#include <memory>
#include <mutex>
#include <string>
#include <cstring>
#include "fss-transport.hpp"

#include <iostream>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <cstring>

#include <cerrno>

#include "transport.hpp"

#ifdef DEBUG
/* Run inet_ntop on a sockaddr_storage object */
static const char *
inet_ntop_stor(struct sockaddr_storage *src, char *dst, size_t dstlen, uint16_t *port)
{
    switch (src->ss_family)
    {
        case AF_INET:
        {
            *port = ntohs(((struct sockaddr_in *)src)->sin_port);
            return inet_ntop (AF_INET, &((struct sockaddr_in *)src)->sin_addr, dst, dstlen);
        } break;
        case AF_INET6:
        {
            *port = ntohs(((struct sockaddr_in6 *)src)->sin6_port);
            return inet_ntop (AF_INET6, &((struct sockaddr_in6 *)src)->sin6_addr, dst, dstlen);
        } break;
    }
    return nullptr;
}
#endif

flight_safety_system::transport::fss_connection::fss_connection() = default;

static void
recv_msg_thread(flight_safety_system::transport::fss_connection *conn)
{
    conn->processMessages();
}

flight_safety_system::transport::fss_connection::fss_connection(int t_fd) : fd(t_fd), recv_thread(std::thread(recv_msg_thread, this))
{
}

void
flight_safety_system::transport::fss_connection::disconnect()
{
    if (this->fd != -1)
    {
        int orig_fd = this->fd;
        this->fd = -1;
        shutdown(orig_fd, 2);
        close(orig_fd);
    }
    if (this->recv_thread.joinable())
    {
        this->recv_thread.join();
    }
}

flight_safety_system::transport::fss_connection::~fss_connection()
{
    this->disconnect();
    while(!this->messages.empty())
    {
        auto msg = this->messages.front();
        this->messages.pop();
    }
}

auto flight_safety_system::transport::fss_connection::getMessageId() -> uint64_t
{
    return ++this->last_msg_id;
}

void
flight_safety_system::transport::fss_connection::processMessages()
{
    this->run = true;
    while (this->run)
    {
        auto msg = this->recvMsg();
        if (msg == nullptr)
        {
            std::cerr << "Got a null msg" << std::endl;
        }
        if (msg && msg->getType() == message_type_closed)
        {
            std::cerr << "Remote closed the connection" << std::endl;
            this->run = false;
        }
        if (this->handler != nullptr)
        {
            this->handler->processMessage(msg);
        }
        else
        {
            this->messages.push(msg);
        }
    }
}

auto
flight_safety_system::transport::fss_connection::getMsg() -> std::shared_ptr<flight_safety_system::transport::fss_message>
{
    if (this->handler == nullptr)
    {
        if (!this->messages.empty())
        {
            auto msg = this->messages.front();
            if (msg != nullptr)
            {
                this->messages.pop();
            }
            return msg;
        }
    }
    return nullptr;
}

auto
flight_safety_system::transport::fss_connection::connectTo(const std::string &address, uint16_t port) -> bool
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

    // Limit the total number of SYN's that are sent
    int synRetries = 2;
    setsockopt(this->fd, IPPROTO_TCP, TCP_SYNCNT, &synRetries, sizeof(synRetries));

    if (connect(this->fd, reinterpret_cast<struct sockaddr *>(&remote), remote.ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)) < 0)
    {
        perror(("Failed to connect to " + address).c_str());
        return false;
    }

    this->recv_thread = std::thread(recv_msg_thread, this);

    return true;
}

auto
flight_safety_system::transport::fss_connection::sendMsg(const std::shared_ptr<fss_message> &msg) -> bool
{
    std::lock_guard<std::mutex> lock_holder(this->send_lock);
    msg->setId(this->getMessageId());
    auto bl = msg->getPacked();
#ifdef DEBUG
    std::cout << "Sending message (len=" << bl->getLength() << ") to " << this->fd << std::endl;
#endif
    if (!bl->isValid())
    {
        return false;
    }
    bool ret = this->sendMsg(bl);
    return ret;
}

#ifdef DEBUG
static void
print_bl(std::shared_ptr<flight_safety_system::transport::buf_len> bl)
{
    unsigned char *data = (unsigned char *)bl->getData();
    size_t len = bl->getLength();
    for(size_t o = 0; o < len; o++)
    {
        printf("0x%04zx: 0x%02x '%1c'\n", o, data[o], data[o]);
    }
    printf("\n");
}
#endif

auto
flight_safety_system::transport::fss_connection::sendMsg(const std::shared_ptr<buf_len> &bl) -> bool
{
    size_t sent = 0;
    size_t to_send = bl->getLength();
    const char *data = bl->getData();
#ifdef DEBUG
    print_bl(bl);
#endif
    while (sent < to_send)
    {
        ssize_t transfered = send(this->fd, &data[sent], to_send - sent, 0);
        if (transfered < 0)
        {
            return false;
        }
        sent += transfered;
    }
    return true;
}

void
flight_safety_system::transport::fss_connection::setHandler(fss_message_cb *cb)
{
    this->handler = cb;
    if (this->handler != nullptr)
    {
        while(!this->messages.empty())
        {
            auto msg = this->messages.front();
            this->messages.pop();
            cb->processMessage(msg);
        }
    }
}

auto
flight_safety_system::transport::fss_connection::recvBytes(void *t_bytes, size_t t_max_bytes) -> ssize_t
{
    return recv(this->fd, t_bytes, t_max_bytes, 0);
}

auto
flight_safety_system::transport::fss_connection::getFd() -> int
{
    return this->fd;
}

void
flight_safety_system::transport::fss_connection::setFd(int new_fd)
{
    this->fd = new_fd;
}

void
flight_safety_system::transport::fss_connection::startRecvThread(std::thread t_recv_thread)
{
    this->recv_thread = std::move(t_recv_thread);
}


auto
flight_safety_system::transport::fss_connection::recvMsg() -> std::shared_ptr<flight_safety_system::transport::fss_message>
{
    std::shared_ptr<flight_safety_system::transport::fss_message> msg = nullptr;
    std::string data;
    data.resize(sizeof (uint16_t));
    ssize_t received = this->recvBytes(&data[0], data.size());
    if (received == static_cast<ssize_t>(data.size()))
    {
        uint16_t data_length = ntohs((reinterpret_cast<const uint16_t *>(data.data()))[0]);
        auto total_length = static_cast<ssize_t>(data_length);
        if (data_length % sizeof(uint64_t) != 0)
        {
            total_length = data_length + sizeof(uint64_t) - (data_length % sizeof(uint64_t));
        }
        /* Get the full message */
        data.resize(total_length);
        while (received != total_length)
        {
            ssize_t this_time = this->recvBytes(&data[received], total_length - received);
            if (this_time < 0)
            {
                perror("Error receiving data");
                break;
            }
            received += this_time;
        }
        if (received == total_length)
        {
            auto bl = std::make_shared<buf_len>(data.data(), data_length);
#ifdef DEBUG
            printf("Message reads: \n");
            print_bl(bl);
#endif
            msg = flight_safety_system::transport::fss_message::decode(bl);
        }
        else
        {
            perror ("Failed to get all the data: ");
        }
    }
    else if(received == 0 || (received < 0 || errno == EBADF))
    {
        /* Connection was closed */
        msg = std::make_shared<flight_safety_system::transport::fss_message_closed>();
    }
    else
    {
        perror("Failed to get header: ");
    }
    return msg;
}

auto
flight_safety_system::transport::fss_connection::getClientNames() -> std::list<std::string>
{
    std::list<std::string> ret;
    return ret;
}

flight_safety_system::transport::fss_listen::fss_listen(uint16_t t_port, fss_connect_cb t_cb) : fss_connection(), port(t_port), cb(t_cb)
{
    this->startListening();
}

flight_safety_system::transport::fss_listen::~fss_listen()
{
    this->disconnect();
}

static void
listen_thread(flight_safety_system::transport::fss_listen *listen)
{
    listen->processMessages();
}

void
flight_safety_system::transport::fss_listen::processMessages()
{
    while (this->getFd() >= 0)
    {
        struct sockaddr_storage sa = {};
        socklen_t sa_len = sizeof(sockaddr_storage);
        int newfd = accept(this->getFd(), reinterpret_cast<struct sockaddr *>(&sa), &sa_len);
        if (newfd < 0)
        {
            if (errno == EBADF)
            {
                return;
            }
            perror("Failed to accept: ");
            continue;
        }
#ifdef DEBUG
        char addr_str[INET6_ADDRSTRLEN];
        uint16_t client_port;
        inet_ntop_stor(&sa, addr_str, INET6_ADDRSTRLEN, &client_port);
        std::cout << "New client from " << addr_str << ":" << client_port << " as " << fd << std::endl;
#endif
        if (this->cb != nullptr)
        {
            auto conn = this->newConnection(newfd);
            this->cb(conn);
        }
        else
        {
            /* Thanks for your call, unfortunately we don't know how to deal with it */
            close(newfd);
        }
    }
}

auto
flight_safety_system::transport::fss_listen::newConnection(int t_newfd) -> std::shared_ptr<flight_safety_system::transport::fss_connection>
{
    return std::make_shared<flight_safety_system::transport::fss_connection>(t_newfd);
}

auto
flight_safety_system::transport::fss_listen::startListening() -> bool
{
    /* open the socket */
    if (this->getFd() < 0)
    {
        this->setFd(socket(PF_INET6, SOCK_STREAM, IPPROTO_TCP));
    }
    struct sockaddr_in6 bind_addr = {};
    memset(&bind_addr, 0, sizeof (struct sockaddr_in6));
    bind_addr.sin6_family = AF_INET6;
    bind_addr.sin6_port = htons(this->port);
    if (bind(this->getFd(), reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr)) < 0)
    {
        perror("Failed to bind socket: ");
        return false;
    }
    if(listen(this->getFd(), this->max_pending_connections) < 0)
    {
        perror("Failed to listen on socket: ");
        return false;
    }
    this->startRecvThread(std::thread(listen_thread, this));
    return true;
}

flight_safety_system::transport::fss_message_cb::~fss_message_cb()
{
    if (this->conn != nullptr)
    {
        this->conn->setHandler(nullptr);
        this->conn.reset();
    }
}