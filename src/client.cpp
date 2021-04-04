#include "fss-client.hpp"
#include "fss-transport.hpp"

#include <iostream>
#include <fstream>
#include <memory>
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
        this->connectTo(config["servers"][idx]["address"].asString(), config["servers"][idx]["port"].asInt(), false);
    }
}

void
flight_safety_system::client::fss_client::connectTo(const std::string &t_address, uint16_t t_port, bool connect)
{
    auto server = std::make_shared<fss_server>(this, t_address, t_port, connect);
    if (server->connected())
    {
        servers.push_back(server);
    }
    else
    {
        reconnect_servers.push_back(server);
    }
}

void
flight_safety_system::client::fss_client::attemptReconnect()
{
    std::list<std::shared_ptr<fss_server>> reconnected;
    bool any_connected = false;
    for (auto const &server : this->reconnect_servers)
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
flight_safety_system::client::fss_client::sendMsgAll(std::shared_ptr<fss_transport::fss_message> msg)
{
    for (auto const &server: this->servers)
    {
        server->getConnection()->sendMsg(msg);
    }
}

void
flight_safety_system::client::fss_client::updateServers(std::shared_ptr<fss_transport::fss_message_server_list> msg)
{
    for (auto const &server_entry: msg->getServers())
    {
        bool exists = false;
        for(auto const &server: this->servers)
        {
            if (server->getAddress().compare(server_entry.first) == 0 && server->getPort() == server_entry.second)
            {
                exists = true;
                break;
            }
        }
        if (!exists)
        {
            for(auto const &server: this->reconnect_servers)
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
            this->connectTo(server_entry.first, server_entry.second, false);
        }
    }
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
flight_safety_system::client::fss_client::serverRequiresReconnect(const std::shared_ptr<fss_server> &server)
{
    this->servers.remove(server);
    this->reconnect_servers.push_back(server);
    this->notifyConnectionStatus();
}

fss_server::fss_server(fss_client *t_client, std::string t_address, uint16_t t_port, bool connect) : fss_message_cb(nullptr), client(t_client), address(std::move(t_address)), port(t_port)
{
    if (connect && this->reconnect())
    {
        this->conn->setHandler(this);
        this->sendIdentify();
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

void
fss_server::sendIdentify()
{
    auto ident_msg = std::make_shared<fss_transport::fss_message_identity>(this->client->getAssetName());
    conn->sendMsg(ident_msg);
}

auto
fss_server::reconnect() -> bool
{
    uint64_t ts = fss_current_timestamp();
    uint64_t elapsed_time = ts - this->last_tried;

    if (this->conn != nullptr)
    {
        delete this->conn;
        this->conn = nullptr;
    }

    if (elapsed_time > this->retry_delay)
    {
        this->retry_count++;
        if (this->retry_delay < retry_delay_cap)
        {
            this->retry_delay += this->retry_delay;
        }
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
            this->sendIdentify();
            this->retry_count = 0;
            this->last_tried = 0;
            this->retry_delay = retry_delay_start;
            return true;
        }
    }
    return false;
}

void
fss_server::processMessage(std::shared_ptr<fss_transport::fss_message> msg)
{
    if (msg == nullptr)
    {
        return;
    }
#ifdef DEBUG
    std::cout << "Got message " << msg->getType() << std::endl;
#endif
    if (msg->getType() == fss_transport::message_type_closed)
    {
        /* Connection has been closed, schedule reconnection */
        this->getClient()->serverRequiresReconnect(std::shared_ptr<fss_server>(this));
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
                auto reply_msg = std::make_shared<fss_transport::fss_message_rtt_response>(msg->getId());
                conn->sendMsg(reply_msg);
            }
                break;
            case fss_transport::message_type_rtt_response:
            /* Currently we don't send any rtt requests */
                break;
            case fss_transport::message_type_position_report:
            {
                /* Servers will be relaying position reports, so this is another asset */
                auto position_msg = std::dynamic_pointer_cast<fss_transport::fss_message_position_report>(msg);
                if (position_msg != nullptr)
                {
                    this->getClient()->handlePositionReport(position_msg);
                }
            } break;
            case fss_transport::message_type_system_status:
            /* Servers don't currently send status reports */
            case fss_transport::message_type_search_status:
            /* Servers don't have a search status to report */
                break;
            case fss_transport::message_type_command:
            {
                auto command_msg = std::dynamic_pointer_cast<fss_transport::fss_message_asset_command>(msg);
                if (command_msg != nullptr)
                {
                    this->getClient()->handleCommand(command_msg);
                }
            } break;
            case fss_transport::message_type_server_list:
            {
                auto server_list_msg = std::dynamic_pointer_cast<fss_transport::fss_message_server_list>(msg);
                if (server_list_msg != nullptr)
                {
                    this->getClient()->updateServers(server_list_msg);
                }
            } break;
            case fss_transport::message_type_smm_settings:
            {
                auto smm_settings_msg = std::dynamic_pointer_cast<fss_transport::fss_message_smm_settings>(msg);
                if (smm_settings_msg != nullptr)
                {
                    this->getClient()->handleSMMSettings(smm_settings_msg);
                }
            } break;
        }
    }
}