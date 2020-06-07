#include "fss-client.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <list>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
#include <json/json.h>
#pragma GCC diagnostic pop

namespace fss_transport = flight_safety_system::transport;

using namespace flight_safety_system::client;

flight_safety_system::client::fss_client::fss_client(const std::string &t_fileName)
{
    /* Open the config file */
    std::ifstream configfile(t_fileName);
    if (!configfile.is_open())
    {
        std::cerr << "Failed to load configuration" << std::endl;
        return;
    }
    Json::Value config;
    configfile >> config;

    this->asset_name = config["name"].asString();

    /* Load all the known servers from the config */
    for (unsigned int idx = 0; idx < config["servers"].size(); idx++)
    {
        this->connectTo(config["servers"][idx]["address"].asString(), config["servers"][idx]["port"].asInt());
    }
}

flight_safety_system::client::fss_client::~fss_client()
{
    while (!this->reconnect_servers.empty())
    {
        auto server = this->reconnect_servers.front();
        this->reconnect_servers.pop_front();
        delete server;
    }
    while (!this->servers.empty())
    {
        auto server = this->servers.front();
        this->servers.pop_front();
        delete server;
    }

}

void
flight_safety_system::client::fss_client::connectTo(const std::string &t_address, uint16_t t_port)
{
    auto conn = new fss_transport::fss_connection();
    if (conn->connectTo(t_address, t_port))
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
    auto server = new fss_server(this, conn, t_address, t_port);
    if (conn == nullptr)
    {
        reconnect_servers.push_back(server);
    }
    else
    {
        servers.push_back(server);
    }
}

void
flight_safety_system::client::fss_client::attemptReconnect()
{
    std::list<fss_server *> reconnected;
    bool any_connected = false;
    for (auto server : this->reconnect_servers)
    {
        if(server->reconnect())
        {
            reconnected.push_back(server);
            any_connected = true;
        }
    }
    while(!reconnected.empty())
    {
        auto server = reconnected.front();
        reconnected.pop_front();
        reconnect_servers.remove(server);
        servers.push_back(server);
    }
    if (any_connected)
    {
        this->notifyConnectionStatus();
    }
}

void
flight_safety_system::client::fss_client::sendMsgAll(fss_transport::fss_message *msg)
{
    for (auto server: this->servers)
    {
        server->getConnection()->sendMsg(msg);
    }
}

void
flight_safety_system::client::fss_client::updateServers(fss_transport::fss_message_server_list *msg)
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
            this->connectTo(server_entry.first, server_entry.second);
        }
    }
}

void
flight_safety_system::client::fss_client::handleCommand(fss_transport::fss_message_asset_command *msg)
{
}

void
flight_safety_system::client::fss_client::handlePositionReport(fss_transport::fss_message_position_report *msg)
{
}

void
flight_safety_system::client::fss_client::connectionStatusChange(flight_safety_system::client::connection_status status)
{
}

void
flight_safety_system::client::fss_client::notifyConnectionStatus()
{
    switch (this->servers.size())
    {
        case 0:
            this->connectionStatusChange(CLIENT_CONNECTION_STATUS_DISCONNECTED);
            break;
        case 1:
            this->connectionStatusChange(CLIENT_CONNECTION_STATUS_CONNECTED_1_SERVER);
            break;
        default:
            this->connectionStatusChange(CLIENT_CONNECTION_STATUS_CONNECTED_2_OR_MORE);
            break;
    }
}

void
flight_safety_system::client::fss_client::serverRequiresReconnect(fss_server *server)
{
    this->servers.remove(server);
    this->reconnect_servers.push_back(server);
    this->notifyConnectionStatus();
}

fss_server::fss_server(fss_client *t_client, fss_transport::fss_connection *t_conn, const std::string &t_address, uint16_t t_port) : fss_message_cb(t_conn), client(t_client), address(t_address), port(t_port)
{
    if (conn != nullptr)
    {
        conn->setHandler(this);
    }
}

fss_server::~fss_server()
{
    if (this->conn != nullptr)
    {
        delete this->conn;
        this->conn = nullptr;
    }
}

bool
fss_server::reconnect()
{
    uint64_t ts = fss_current_timestamp();
    bool try_now = false;
    uint64_t elapsed_time = ts - this->last_tried;

    if (this->conn != nullptr)
    {
        delete this->conn;
        this->conn = nullptr;
    }

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
        if (!this->conn->connectTo(this->getAddress(), this->getPort()))
        {
            delete this->conn;
            this->conn = nullptr;
        }
        else
        {
            conn->setHandler(this);
            auto ident_msg = new fss_transport::fss_message_identity(this->client->getAssetName());
            conn->sendMsg(ident_msg);
            delete ident_msg;
            this->retry_count = 0;
            this->last_tried = 0;
            return true;
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
        this->getClient()->serverRequiresReconnect(this);
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
            case fss_transport::message_type_identity_non_aircraft:
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
            {
                /* Servers will be relaying position reports, so this is another asset */
                this->getClient()->handlePositionReport((fss_transport::fss_message_position_report *)msg);
            } break;
            case fss_transport::message_type_system_status:
            /* Servers don't currently send status reports */
                break;
            case fss_transport::message_type_search_status:
            /* Servers don't have a search status to report */
                break;
            case fss_transport::message_type_command:
            {
                this->getClient()->handleCommand((fss_transport::fss_message_asset_command *)msg);
            } break;
            case fss_transport::message_type_server_list:
            {
                this->getClient()->updateServers((fss_transport::fss_message_server_list *)msg);
            } break;
            case fss_transport::message_type_smm_settings:
            {
                /* TODO */
            } break;
        }
    }
}
