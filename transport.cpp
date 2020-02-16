#include <string>
#include <cstring>
#include "fss-internal.hpp"

#include <iostream>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>

#include <errno.h>

#include <sys/time.h>

#include "transport.hpp"

fss::fss(std::string name)
{
    this->name = name;
}

fss_connection *fss::connect(std::string address, uint16_t port)
{
    fss_connection *conn = new fss_connection();
    if (!conn->connect_to(address, port))
    {
        delete conn;
        return NULL;
    }
    fss_message *msg = new fss_message_identity(conn->getMessageId(), this->getName());
    conn->sendMsg(msg);
    delete msg;
    return conn;
}

void
recv_msg_thread(fss_connection *conn)
{
    conn->processMessages();
}

fss_connection::fss_connection()
{
    this->fd = socket(PF_INET6, SOCK_STREAM, IPPROTO_TCP);
    this->handler = NULL;
    this->last_msg_id = 0;
}

fss_connection::fss_connection(int fd)
{
    this->fd = fd;
    this->handler = NULL;
    this->last_msg_id = 0;
    this->recv_thread = std::thread(recv_msg_thread, this);
}

fss_connection::~fss_connection()
{
    shutdown(this->fd, 2);
    close(this->fd);
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

uint64_t fss_connection::getMessageId()
{
    return ++this->last_msg_id;
}

void
fss_connection::processMessages()
{
    bool run = true;
    while (run)
    {
        fss_message *msg = this->recvMsg();
        if (msg && msg->getType() == message_type_closed)
        {
            run = false;
        }
        if (this->handler != NULL)
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

bool fss_connection::connect_to(std::string address, uint16_t port)
{
    struct sockaddr_storage remote;
    if (!convert_str_to_sa (address, port, &remote))
    {
        std::cerr << "Failed to convert '" << address << "' to a usable address\n";
        return false;
    }

    if (connect(this->fd, (struct sockaddr *)&remote, remote.ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)) < 0)
    {
        perror("Failed to connect: ");
        return false;
    }

    this->recv_thread = std::thread(recv_msg_thread, this);

    return true;
}

bool
fss_connection::sendMsg(fss_message *msg)
{
    buf_len bl = msg->getPacked();
    if (!bl.isValid())
    {
        return false;
    }
    return this->sendMsg(bl);
}

#ifdef DEBUG
static void
print_bl(buf_len &bl)
{
    char *data = bl.getData();
    size_t len = bl.getLength();
    for(size_t o = 0; o < len; o++)
    {
        printf("0x%04zx: 0x%02x '%1c'\n", o, data[o], data[o]);
    }
    printf("\n");
}
#endif

bool
fss_connection::sendMsg(buf_len &bl)
{
    size_t sent = 0;
    size_t to_send = bl.getLength();
    const char *data = bl.getData();
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

void fss_connection::setHandler(fss_message_cb *cb)
{
    this->handler = cb;
    while(!this->messages.empty())
    {
        fss_message *msg = this->messages.front();
        this->messages.pop();
        cb->processMessage(msg);
        delete msg;
    }
}

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
    return NULL;
}
#endif

fss_message *
fss_connection::recvMsg()
{
    fss_message *msg = NULL;
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
            buf_len bl(data, data_length);
#ifdef DEBUG
            printf("Message reads: \n");
            print_bl(bl);
#endif
            msg = fss_message::decode(bl);
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
        msg = new fss_message_closed();
    }
    else
    {
        perror("Failed to get header: ");
    }
    free (header_data);
    return msg;
}

static void
listen_thread(fss_listen *listen)
{
    listen->processMessages();
}

void
fss_listen::processMessages()
{
    while (1)
    {
        struct sockaddr_storage sa = {};
        socklen_t sa_len = sizeof(sizeof(sockaddr_storage));
        int fd = accept(this->fd, (struct sockaddr *)&sa, &sa_len);
        if (fd < 0)
        {
            perror("Failed to accept: ");
            if (errno == EBADF)
            {
                return;
            }
            continue;
        }
#ifdef DEBUG
        char addr_str[INET6_ADDRSTRLEN];
        uint16_t port;
        inet_ntop_stor(&sa, addr_str, INET6_ADDRSTRLEN, &port);
        std::cout << "New client from " << addr_str << ":" << port << " as " << fd << std::endl;
#endif
        if (this->cb != NULL)
        {
            fss_connection *conn = new fss_connection(fd);
            if(!this->cb(conn))
            {
                delete conn;
            }
        }
        else
        {
            /* Thanks for your call, unfortunately we don't know how to deal with it */
            close(fd);
        }
    }
}

bool
fss_listen::startListening()
{
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

uint64_t
flight_safety_system::fss_current_timestamp()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + (tv.tv_usec / 1000);
}
