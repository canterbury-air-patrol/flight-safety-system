#include "fss-internal.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <list>
#include <csignal>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
#include <json/json.h>
#pragma GCC diagnostic pop

#include <unistd.h>

bool running = true;

class fss_server: public fss_message_cb {
protected:
    std::string address;
    uint16_t port;
    uint64_t last_tried;
    uint64_t retry_count;
public:
    fss_server(fss_connection *t_conn, std::string t_address, uint16_t t_port);
    virtual ~fss_server();
    virtual void processMessage(fss_message *message) override;
    virtual std::string getAddress() { return this->address; };
    virtual uint16_t getPort() { return this->port; };
    virtual bool reconnect();
};

std::list<fss_server *> servers;
std::list<fss_server *> reconnect_servers;

void
updateServers(fss_message_server_list *msg)
{
    for (auto server_entry: msg->getServers())
    {
        bool exists = false;
        for(auto server: servers)
        {
            if (server->getAddress() == server_entry.first && server->getPort() == server_entry.second)
            {
                exists = true;
                break;
            }
        }
        if (!exists)
        {
            for(auto server: servers)
            {
                if(server->getAddress() == server_entry.first && server->getPort() == server_entry.second)
                {
                    exists = true;
                    break;
                }
            }
        }
        if (!exists)
        {
            fss_connection *conn = new fss_connection();
            if (!conn->connect_to(server_entry.first, server_entry.second))
            {
                delete conn;
                conn = nullptr;
            }
            fss_server *server = new fss_server(conn, server_entry.first, server_entry.second);
            if (conn == nullptr)
            {
                reconnect_servers.push_back(server);
            }
            else
            {
                servers.push_back(server);
            }
        }
    }
}

fss_server::fss_server(fss_connection *t_conn, std::string t_address, uint16_t t_port) : fss_message_cb(t_conn), address(t_address), port(t_port), last_tried(0), retry_count(0)
{
    if (conn != nullptr)
    {
        conn->setHandler(this);
    }
}

fss_server::~fss_server()
{
    if (this->conn != NULL)
    {
        delete this->conn;
        this->conn = NULL;
    }
}

bool
fss_server::reconnect()
{
    if (this->conn == nullptr)
    {
        uint64_t ts = fss_current_timestamp();
        bool try_now = false;
        uint64_t elapsed_time = ts - this->last_tried;
        switch (this->retry_count)
        {
            case 0:
                try_now = (elapsed_time > 1000);
                break;
            case 1:
                try_now = (elapsed_time > 2000);
                break;
            case 2:
                try_now = (elapsed_time > 4000);
                break;
            case 3:
                try_now = (elapsed_time > 8000);
                break;
            case 4:
                try_now = (elapsed_time > 15000);
                break;
            default:
                try_now = (elapsed_time > 30000);
                break;
        }
        if (try_now)
        {
            this->retry_count++;
            this->last_tried = ts;
            this->conn = new fss_connection();
            if (!this->conn->connect_to(this->getAddress(), this->getPort()))
            {
                delete this->conn;
                this->conn = nullptr;
            }
            else
            {
                conn->setHandler(this);
                return true;
            }
        }
    }
    return false;
}

void
fss_server::processMessage(fss_message *msg)
{
#ifdef DEBUG
    std::cout << "Got message " << msg->getType() << std::endl;
#endif
    if (msg->getType() == message_type_closed)
    {
        /* Connection has been closed, schedule reconnection */
        servers.remove(this);
        reconnect_servers.push_back(this);
        this->last_tried = 0;
        this->retry_count = 0;
        return;
    }
    else
    {
        switch (msg->getType())
        {
            case message_type_unknown:
            case message_type_closed:
            case message_type_identity:
                break;
            case message_type_rtt_request:
            {
                /* Send a response */
                fss_message_rtt_response *reply_msg = new fss_message_rtt_response(msg->getId());
                conn->sendMsg(reply_msg);
                delete reply_msg;
            }
                break;
            case message_type_rtt_response:
            /* Currently we don't send any rtt requests */
                break;
            case message_type_position_report:
            /* Servers will be relaying position reports, so this is another asset */
                break;
            case message_type_system_status:
            /* Servers don't currently send status reports */
                break;
            case message_type_search_status:
            /* Servers don't have a search status to report */
                break;
            case message_type_command:
            {
                /* TODO */
            }
                break;
            case message_type_server_list:
            {
                updateServers((fss_message_server_list *)msg);
            }
                break;
            case message_type_smm_settings:
            {
                /* TODO */
            }
                break;
        }
    }
}

void sigIntHandler(int signum)
{
    running = false;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        std::cout << "Insufficient arguments" << std::endl;
        return -1;
    }
    /* Watch out for sigint */
    signal (SIGINT, sigIntHandler);

    /* Load all the known servers from the config */
    std::ifstream configfile(argv[1]);
    if (!configfile.is_open())
    {
        printf("Failed to load configuration\n");
        return -1;
    }
    Json::Value config;
    configfile >> config;

    fss connection = fss(config["name"].asString());

    for (unsigned int idx = 0; idx < config["servers"].size(); idx++)
    {
        fss_connection *conn = connection.connect(config["servers"][idx]["address"].asString(), config["servers"][idx]["port"].asInt());
        if (conn == NULL)
        {
            return -1;
        }
        servers.push_back(new fss_server(conn, config["servers"][idx]["address"].asString(), config["servers"][idx]["port"].asInt()));
    }

    /* Send each server some fake details */
    fss_message_system_status *msg_status = new fss_message_system_status(75, 1000);
    fss_message_search_status *msg_search = new fss_message_search_status(1, 23, 100);
    fss_message_position_report *msg_pos = new fss_message_position_report(-43.5, 172.5, 300, fss_current_timestamp());
    for (auto server: servers)
    {
        server->getConnection()->sendMsg(msg_status);
        server->getConnection()->sendMsg(msg_search);
        server->getConnection()->sendMsg(msg_pos);
    }
    delete msg_status;
    delete msg_search;
    delete msg_pos;
    /* Connect to each server */
    /* Send reports:
       - Battery status
       - Position
       - Search information
     */
    /* Receieve:
       - Commands
       - ping request
       - Position reports
       - Updated server list
     */
    /* Possible events:
       - Server reset
     */
    
    while (running)
    {
        sleep (1);
        std::list<fss_server *> reconnected;
        for (auto server : reconnect_servers)
        {
            if(server->reconnect())
            {
                reconnected.push_back(server);
            }
        }
        while(!reconnected.empty())
        {
            auto server = reconnected.front();
            reconnected.pop_front();
            reconnect_servers.remove(server);
            servers.push_back(server);
        }
    }
    while (!reconnect_servers.empty())
    {
        auto server = reconnect_servers.front();
        reconnect_servers.pop_front();
        delete server;
    }
    while (!servers.empty())
    {
        auto server = servers.front();
        servers.pop_front();
        delete server;
    }
}
