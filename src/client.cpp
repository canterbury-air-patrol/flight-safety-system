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
        auto server = std::make_shared<fss_server>(this, config["servers"][idx]["address"].asString(), config["servers"][idx]["port"].asInt());
        this->addServer(server);
    }
}

void
flight_safety_system::client::fss_client::disconnect()
{
    for (const auto &server : this->servers)
    {
        server->disconnect();
    }
}

void
flight_safety_system::client::fss_client::addServer(const std::shared_ptr<fss_server> &server)
{
    if (server->connected())
    {
        this->servers.push_back(server);
    }
    else
    {
        this->reconnect_servers.push_back(server);
    }
}

void
flight_safety_system::client::fss_client::connectTo(const std::string &t_address, uint16_t t_port, bool t_connect)
{
    auto server = std::make_shared<fss_server>(this, t_address, t_port);
    if (t_connect)
    {
        server->reconnect();
    }
    this->addServer(server);
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
flight_safety_system::client::fss_client::sendMsgAll(const std::shared_ptr<fss_transport::fss_message> &msg)
{
    for (auto const &server: this->servers)
    {
        server->sendMsg(msg);
    }
}

static auto
server_list_matches (const std::list<std::shared_ptr<flight_safety_system::client::fss_server>> &servers, const std::string &address, uint16_t port) -> bool
{
    for(auto const &server: servers)
    {
        if (server->getAddress().compare(address) == 0 && server->getPort() == port)
        {
            return true;
        }
    }
    return false;
}

void
flight_safety_system::client::fss_client::updateServers(const std::shared_ptr<fss_transport::fss_message_server_list> &msg)
{
    for (auto const &server_entry: msg->getServers())
    {
        bool exists = false;
        exists = server_list_matches(this->servers, server_entry.first, server_entry.second);
        if (!exists)
        {
            exists = server_list_matches(this->reconnect_servers, server_entry.first, server_entry.second);
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
flight_safety_system::client::fss_client::serverRequiresReconnect(fss_server *server)
{
    for (auto s : this->servers)
    {
        if (s.get() == server)
        {
            this->reconnect_servers.push_back(std::move(s));
            this->servers.remove(s);
            break;
        }
    }
    this->notifyConnectionStatus();
}

flight_safety_system::client::fss_server::fss_server(fss_client *t_client, std::string t_address, uint16_t t_port) : fss_message_cb(nullptr), client(t_client), address(std::move(t_address)), port(t_port)
{
}

void
flight_safety_system::client::fss_server::sendIdentify()
{
    auto ident_msg = std::make_shared<fss_transport::fss_message_identity>(this->client->getAssetName());
    this->getConnection()->sendMsg(ident_msg);
}

auto
flight_safety_system::client::fss_server::reconnect_to() -> bool
{
    this->setConnection(std::make_shared<fss_transport::fss_connection>());
    return this->getConnection()->connectTo(this->getAddress(), this->getPort());
}

auto
flight_safety_system::client::fss_server::reconnect() -> bool
{
    uint64_t ts = fss_current_timestamp();
    uint64_t elapsed_time = ts - this->last_tried;

    if (this->getConnection() != nullptr)
    {
        this->clearConnection();
    }

    if (elapsed_time > this->retry_delay)
    {
        this->retry_count++;
        if (this->retry_delay < retry_delay_cap)
        {
            this->retry_delay += this->retry_delay;
        }
        this->last_tried = ts;
        if (!this->reconnect_to())
        {
            this->clearConnection();
        }
        else
        {
            this->getConnection()->setHandler(this);
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
flight_safety_system::client::fss_server::processMessage(std::shared_ptr<fss_transport::fss_message> msg)
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
            case fss_transport::message_type_identity_required:
                break;
            case fss_transport::message_type_rtt_request:
            {
                /* Send a response */
                auto reply_msg = std::make_shared<fss_transport::fss_message_rtt_response>(msg->getId());
                this->sendMsg(reply_msg);
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