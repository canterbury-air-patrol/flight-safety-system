#include <fss-client-ssl.hpp>

#include <iostream>
#include <fstream>
#include <ostream>
#include <string>
#include <utility>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
#include <json/json.h>
#pragma GCC diagnostic pop

flight_safety_system::client_ssl::fss_client::fss_client(const std::string &t_fileName)
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

    this->setAssetName(config["name"].asString());

    this->ca_file = config["ssl"]["ca_public_key"].asString();
    this->private_key_file = config["ssl"]["client_private_key"].asString();
    this->public_key_file = config["ssl"]["client_public_key"].asString();

    /* Load all the known servers from the config */
    for (unsigned int idx = 0; idx < config["servers"].size(); idx++)
    {
        auto server = std::make_shared<flight_safety_system::client_ssl::fss_server>(this, config["servers"][idx]["address"].asString(), config["servers"][idx]["port"].asInt(), this->ca_file, this->private_key_file, this->public_key_file);
        this->addServer(server);
    }
}

flight_safety_system::client_ssl::fss_client::fss_client() = default;

flight_safety_system::client_ssl::fss_client::fss_client(std::string t_ca, std::string t_private_key, std::string t_public_key) : ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)), public_key_file(std::move(t_public_key))
{
}

flight_safety_system::client_ssl::fss_client::~fss_client() = default;

void
flight_safety_system::client_ssl::fss_client::disconnect()
{
    for (const auto &server : this->servers)
    {
        server->disconnect();
    }
}

void
flight_safety_system::client_ssl::fss_client::setAssetName(std::string t_asset_name) {
    this->asset_name = std::move(t_asset_name);
}

void
flight_safety_system::client_ssl::fss_client::connectTo(const std::string &t_address, uint16_t t_port, bool t_connect)
{
    auto server = std::make_shared<flight_safety_system::client_ssl::fss_server>(this, t_address, t_port, this->ca_file, this->private_key_file, this->public_key_file);
    if (t_connect)
    {
        server->reconnect();
    }
    this->addServer(server);
}

void
flight_safety_system::client_ssl::fss_client::attemptReconnect()
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
flight_safety_system::client_ssl::fss_client::sendMsgAll(const std::shared_ptr<flight_safety_system::transport::fss_message> &msg)
{
    for (auto const &server: this->servers)
    {
        server->sendMsg(msg);
    }
}

auto
flight_safety_system::client_ssl::fss_client::getAssetName() -> std::string
{
    return this->asset_name;
}

void
flight_safety_system::client_ssl::fss_client::addServer(const std::shared_ptr<flight_safety_system::client_ssl::fss_server> &server)
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

static auto
server_list_matches (const std::list<std::shared_ptr<flight_safety_system::client_ssl::fss_server>> &servers, const std::string &address, uint16_t port) -> bool
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
flight_safety_system::client_ssl::fss_client::updateServers(const std::shared_ptr<flight_safety_system::transport::fss_message_server_list> &msg)
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
flight_safety_system::client_ssl::fss_client::serverRequiresReconnect(flight_safety_system::client_ssl::fss_server *server)
{
    for (auto s : this->servers)
    {
        if (s.get() == server)
        {
            this->servers.remove(s);
            this->reconnect_servers.push_back(std::move(s));
            break;
        }
    }
    this->notifyConnectionStatus();
}

void
flight_safety_system::client_ssl::fss_client::notifyConnectionStatus()
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
flight_safety_system::client_ssl::fss_client::connectionStatusChange(flight_safety_system::client_ssl::connection_status status __attribute__((unused)))
{
}

void
flight_safety_system::client_ssl::fss_client::handleCommand(const std::shared_ptr<flight_safety_system::transport::fss_message_asset_command> &msg __attribute__((unused)))
{
}

void
flight_safety_system::client_ssl::fss_client::handlePositionReport(const std::shared_ptr<flight_safety_system::transport::fss_message_position_report> &msg __attribute__((unused)))
{
}

void
flight_safety_system::client_ssl::fss_client::handleSMMSettings(const std::shared_ptr<flight_safety_system::transport::fss_message_smm_settings> &msg __attribute__((unused)))
{
}

flight_safety_system::client_ssl::fss_server::fss_server(flight_safety_system::client_ssl::fss_client *t_client, std::string t_address, uint16_t t_port, std::string t_ca, std::string t_private_key, std::string t_public_key) : flight_safety_system::transport::fss_message_cb(nullptr), client(t_client), address(std::move(t_address)), port(t_port), ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)), public_key_file(std::move(t_public_key))
{
}

flight_safety_system::client_ssl::fss_server::~fss_server() = default;

auto
flight_safety_system::client_ssl::fss_server::getAddress() -> std::string
{
    return this->address;
}

auto
flight_safety_system::client_ssl::fss_server::getPort() -> uint16_t
{
    return this->port;
}

auto
flight_safety_system::client_ssl::fss_server::getClient() -> fss_client *
{
    return this->client;
}


void
flight_safety_system::client_ssl::fss_server::sendIdentify()
{
    auto ident_msg = std::make_shared<flight_safety_system::transport::fss_message_identity>(this->client->getAssetName());
    this->getConnection()->sendMsg(ident_msg);
}

auto
flight_safety_system::client_ssl::fss_server::reconnect_to() -> bool
{
    this->setConnection(std::make_shared<flight_safety_system::transport_ssl::fss_connection_client>(this->ca_file, this->private_key_file, this->public_key_file));
    return this->getConnection()->connectTo(this->getAddress(), this->getPort());
}

auto
flight_safety_system::client_ssl::fss_server::reconnect() -> bool
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
flight_safety_system::client_ssl::fss_server::processMessage(std::shared_ptr<flight_safety_system::transport::fss_message> msg)
{
    if (msg == nullptr)
    {
        return;
    }
#ifdef DEBUG
    std::cout << "Got message " << msg->getType() << std::endl;
#endif
    if (msg->getType() == flight_safety_system::transport::message_type_closed)
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
            case flight_safety_system::transport::message_type_unknown:
            case flight_safety_system::transport::message_type_closed:
            case flight_safety_system::transport::message_type_identity:
            case flight_safety_system::transport::message_type_identity_non_aircraft:
            case flight_safety_system::transport::message_type_identity_required:
                break;
            case flight_safety_system::transport::message_type_rtt_request:
            {
                /* Send a response */
                auto reply_msg = std::make_shared<flight_safety_system::transport::fss_message_rtt_response>(msg->getId());
                this->sendMsg(reply_msg);
            }
                break;
            case flight_safety_system::transport::message_type_rtt_response:
            /* Currently we don't send any rtt requests */
                break;
            case flight_safety_system::transport::message_type_position_report:
            {
                /* Servers will be relaying position reports, so this is another asset */
                auto position_msg = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_position_report>(msg);
                if (position_msg != nullptr)
                {
                    this->getClient()->handlePositionReport(position_msg);
                }
            } break;
            case flight_safety_system::transport::message_type_system_status:
            /* Servers don't currently send status reports */
            case flight_safety_system::transport::message_type_search_status:
            /* Servers don't have a search status to report */
                break;
            case flight_safety_system::transport::message_type_command:
            {
                auto command_msg = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_asset_command>(msg);
                if (command_msg != nullptr)
                {
                    this->getClient()->handleCommand(command_msg);
                }
            } break;
            case flight_safety_system::transport::message_type_server_list:
            {
                auto server_list_msg = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_server_list>(msg);
                if (server_list_msg != nullptr)
                {
                    this->getClient()->updateServers(server_list_msg);
                }
            } break;
            case flight_safety_system::transport::message_type_smm_settings:
            {
                auto smm_settings_msg = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_smm_settings>(msg);
                if (smm_settings_msg != nullptr)
                {
                    this->getClient()->handleSMMSettings(smm_settings_msg);
                }
            } break;
        }
    }
}