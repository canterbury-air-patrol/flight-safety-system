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

namespace fss_transport = flight_safety_system::transport;

bool running = true;
std::string asset_name;

namespace flight_safety_system {
namespace client {
class fss_server: public fss_transport::fss_message_cb {
protected:
    std::string address;
    uint16_t port;
    uint64_t last_tried;
    uint64_t retry_count;
public:
    fss_server(fss_transport::fss_connection *t_conn, const std::string &t_address, uint16_t t_port);
    virtual ~fss_server();
    virtual void processMessage(fss_transport::fss_message *message) override;
    virtual std::string getAddress() { return this->address; };
    virtual uint16_t getPort() { return this->port; };
    virtual bool reconnect();
};
}
}

using namespace flight_safety_system::client;

std::list<fss_server *> servers;
std::list<fss_server *> reconnect_servers;

void
updateServers(fss_transport::fss_message_server_list *msg)
{
    for (auto server_entry: msg->getServers())
    {
        bool exists = false;
        for(auto server: servers)
        {
            if (server->getAddress().compare(server_entry.first) == 0 && server->getPort() == server_entry.second)
            {
                exists = true;
                break;
            }
        }
        if (!exists)
        {
            for(auto server: servers)
            {
                if(server->getAddress().compare(server_entry.first) == 0 && server->getPort() == server_entry.second)
                {
                    exists = true;
                    break;
                }
            }
        }
        if (!exists)
        {
            auto conn = new fss_transport::fss_connection();
            if (conn->connect_to(server_entry.first, server_entry.second))
            {
                auto ident_msg = new fss_transport::fss_message_identity(asset_name);
                conn->sendMsg(ident_msg);
                delete ident_msg;
            }
            else
            {
                delete conn;
                conn = nullptr;
            }
            auto server = new fss_server(conn, server_entry.first, server_entry.second);
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

fss_server::fss_server(fss_transport::fss_connection *t_conn, const std::string &t_address, uint16_t t_port) : fss_message_cb(t_conn), address(t_address), port(t_port), last_tried(0), retry_count(0)
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
            this->conn = new fss_transport::fss_connection();
            if (!this->conn->connect_to(this->getAddress(), this->getPort()))
            {
                delete this->conn;
                this->conn = nullptr;
            }
            else
            {
                conn->setHandler(this);
                auto ident_msg = new fss_transport::fss_message_identity(asset_name);
                conn->sendMsg(ident_msg);
                delete ident_msg;
                return true;
            }
        }
    }
    return false;
}

void
fss_server::processMessage(fss_transport::fss_message *msg)
{
#ifdef DEBUG
    std::cout << "Got message " << msg->getType() << std::endl;
#endif
    if (msg->getType() == fss_transport::message_type_closed)
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
            case fss_transport::message_type_unknown:
            case fss_transport::message_type_closed:
            case fss_transport::message_type_identity:
                break;
            case fss_transport::message_type_rtt_request:
            {
                /* Send a response */
                auto reply_msg = new fss_transport::fss_message_rtt_response(msg->getId());
                conn->sendMsg(reply_msg);
                delete reply_msg;
            }
                break;
            case fss_transport::message_type_rtt_response:
            /* Currently we don't send any rtt requests */
                break;
            case fss_transport::message_type_position_report:
            /* Servers will be relaying position reports, so this is another asset */
                break;
            case fss_transport::message_type_system_status:
            /* Servers don't currently send status reports */
                break;
            case fss_transport::message_type_search_status:
            /* Servers don't have a search status to report */
                break;
            case fss_transport::message_type_command:
            {
                /* TODO */
            }
                break;
            case fss_transport::message_type_server_list:
            {
                updateServers((fss_transport::fss_message_server_list *)msg);
            }
                break;
            case fss_transport::message_type_smm_settings:
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

    asset_name = config["name"].asString();
    auto connection = flight_safety_system::fss(asset_name);

    for (unsigned int idx = 0; idx < config["servers"].size(); idx++)
    {
        auto conn = connection.connect(config["servers"][idx]["address"].asString(), config["servers"][idx]["port"].asInt());
        if (conn == NULL)
        {
            return -1;
        }
        servers.push_back(new fss_server(conn, config["servers"][idx]["address"].asString(), config["servers"][idx]["port"].asInt()));
    }

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
    
    int counter = 0;
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
        counter++;
        if (counter % 5 == 0)
        {
            /* Send each server some fake details */
            auto msg_status = new fss_transport::fss_message_system_status(75, 1000);
            auto msg_search = new fss_transport::fss_message_search_status(1, 23, 100);
            auto msg_pos = new fss_transport::fss_message_position_report(-43.5, 172.5, 300, flight_safety_system::fss_current_timestamp());
            for (auto server: servers)
            {
                server->getConnection()->sendMsg(msg_status);
                server->getConnection()->sendMsg(msg_search);
                server->getConnection()->sendMsg(msg_pos);
            }
            delete msg_status;
            delete msg_search;
            delete msg_pos;
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
