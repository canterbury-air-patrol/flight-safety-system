#include <string>
#include <cstring>
#include "fss-transport.hpp"

#include <iostream>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>

#include <errno.h>

#include "transport.hpp"

namespace fss_transport = flight_safety_system::transport;

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

static void
recv_msg_thread(fss_transport::fss_connection *conn)
{
    conn->processMessages();
}

fss_transport::fss_connection::fss_connection(int t_fd) : fd(t_fd), last_msg_id(0), handler(nullptr), messages(), recv_thread(std::thread(recv_msg_thread, this)), send_lock()
{
}

void
fss_transport::fss_connection::disconnect()
{
    if (this->fd != -1)
    {
        shutdown(this->fd, 2);
        close(this->fd);
        this->fd = -1;
    }
    if (this->recv_thread.joinable())
    {
        this->recv_thread.join();
    }
}

fss_transport::fss_connection::~fss_connection()
{
    if (this->recv_thread.joinable())
    {
        this->recv_thread.join();
    }
    while(!this->messages.empty())
    {
        fss_message *msg = this->messages.front();
        this->messages.pop();
        delete msg;
    }
}

uint64_t fss_transport::fss_connection::getMessageId()
{
    return ++this->last_msg_id;
}

void
fss_transport::fss_connection::processMessages()
{
    bool run = true;
    while (run)
    {
        auto msg = this->recvMsg();
        if (msg && msg->getType() == message_type_closed)
        {
            run = false;
        }
        if (this->handler != nullptr)
        {
            this->handler->processMessage(msg);
            delete msg;
        }
        else
        {
            this->messages.push(msg);
        }
    }
}

fss_transport::fss_message *
fss_transport::fss_connection::getMsg()
{
    if (this->handler == nullptr)
    {
        auto msg = this->messages.front();
        if (msg != nullptr)
        {
            this->messages.pop();
        }
        return msg;
    }
    return nullptr;
}

bool fss_transport::fss_connection::connectTo(const std::string &address, uint16_t port)
{
    struct sockaddr_storage remote;
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

    this->recv_thread = std::thread(recv_msg_thread, this);

    return true;
}

bool
fss_transport::fss_connection::sendMsg(fss_message *msg)
{
    this->send_lock.lock();
    msg->setId(this->getMessageId());
    auto bl = msg->getPacked();
#ifdef DEBUG
    std::cout << "Sending message (len=" << bl->getLength() << ") to " << this->fd << std::endl;
#endif
    if (!bl->isValid())
    {
        delete bl;
        return false;
    }
    bool ret = this->sendMsg(bl);
    delete bl;
    this->send_lock.unlock();
    return ret;
}

#ifdef DEBUG
static void
print_bl(fss_transport::buf_len *bl)
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

bool
fss_transport::fss_connection::sendMsg(buf_len *bl)
{
    size_t sent = 0;
    size_t to_send = bl->getLength();
    const char *data = bl->getData();
#ifdef DEBUG
    print_bl(bl);
#endif
    while (sent < to_send)
    {
        ssize_t transfered = send(this->fd, data + sent, to_send - sent, 0);
        if (transfered < 0)
        {
            return false;
        }
        sent += transfered;
    }
    return true;
}

void fss_transport::fss_connection::setHandler(fss_message_cb *cb)
{
    this->handler = cb;
    while(!this->messages.empty())
    {
        auto msg = this->messages.front();
        this->messages.pop();
        cb->processMessage(msg);
        delete msg;
    }
}

fss_transport::fss_message *
fss_transport::fss_connection::recvMsg()
{
    fss_transport::fss_message *msg = nullptr;
    size_t header_len = sizeof (uint16_t);
    char *header_data = (char *) malloc(header_len);
    ssize_t received = recv(this->fd, header_data, header_len, 0);
    if (received == (ssize_t) header_len)
    {
        uint16_t data_length = ntohs(*(uint16_t *)header_data);
        ssize_t total_length = (ssize_t) data_length;
        if (data_length % sizeof(uint64_t) != 0)
        {
            total_length = data_length + sizeof(uint64_t) - (data_length % sizeof(uint64_t));
        }
        /* Get the full message */
        char *data = (char *) malloc (total_length);
        memcpy(data, header_data, header_len);
        while (received != total_length)
        {
            ssize_t this_time = recv(this->fd, data + received, total_length - received, 0);
            if (this_time < 0)
            {
                perror("Error receiving data");
                break;
            }
            received += this_time;
        }
        if (received == total_length)
        {
            buf_len *bl = new buf_len(data, data_length);
#ifdef DEBUG
            printf("Message reads: \n");
            print_bl(bl);
#endif
            msg = fss_transport::fss_message::decode(bl);
            delete bl;
        }
        else
        {
            perror ("Failed to get all the data: ");
            free (data);
        }
    }
    else if(received == 0 || (received < 0 || errno == EBADF))
    {
        /* Connection was closed */
        msg = new fss_transport::fss_message_closed();
    }
    else
    {
        perror("Failed to get header: ");
    }
    free (header_data);
    return msg;
}

fss_transport::fss_listen::~fss_listen()
{
    if (this->fd != -1)
    {
        shutdown(this->fd, 2);
        close (this->fd);
    }
    if (this->recv_thread.joinable())
    {
        this->recv_thread.join();
    }
}

static void
listen_thread(fss_transport::fss_listen *listen)
{
    listen->processMessages();
}

void
fss_transport::fss_listen::processMessages()
{
    while (this->fd >= 0)
    {
        struct sockaddr_storage sa = {};
        socklen_t sa_len = sizeof(sockaddr_storage);
        int newfd = accept(this->fd, (struct sockaddr *)&sa, &sa_len);
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
            auto conn = new fss_transport::fss_connection(newfd);
            if(!this->cb(conn))
            {
                delete conn;
            }
        }
        else
        {
            /* Thanks for your call, unfortunately we don't know how to deal with it */
            close(newfd);
        }
    }
}

bool
fss_transport::fss_listen::startListening()
{
    /* open the socket */
    if (this->fd < 0)
    {
        this->fd = socket(PF_INET6, SOCK_STREAM, IPPROTO_TCP);
    }
    struct sockaddr_in6 bind_addr;
    memset(&bind_addr, 0, sizeof (struct sockaddr_in6));
    bind_addr.sin6_family = AF_INET6;
    bind_addr.sin6_port = htons(this->port);
    if (bind(this->fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0)
    {
        perror("Failed to bind socket: ");
        return false;
    }
    if(listen(this->fd, 10) < 0)
    {
        perror("Failed to listen on socket: ");
        return false;
    }
    this->recv_thread = std::thread(listen_thread, this);
    return true;
}
