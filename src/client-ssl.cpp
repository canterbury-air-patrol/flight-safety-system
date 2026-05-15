#include <fss-client-ssl.hpp>
#include "fss-log.hpp"

#include <algorithm>
#include <iostream>
#include <fstream>
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
        FSS_LOG_ERROR("client", "Failed to load configuration");
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
        auto server = std::make_shared<flight_safety_system::client_ssl::fss_server>(
            this, config["servers"][idx]["address"].asString(), config["servers"][idx]["port"].asInt(), this->ca_file,
            this->private_key_file, this->public_key_file);
        this->addServer(server);
    }
}

flight_safety_system::client_ssl::fss_client::fss_client() = default;

flight_safety_system::client_ssl::fss_client::fss_client(std::string t_ca, std::string t_private_key,
                                                         std::string t_public_key)
    : ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)), public_key_file(std::move(t_public_key))
{
}

flight_safety_system::client_ssl::fss_client::~fss_client()
{
    // Drain both lists before disconnecting so that any concurrent
    // serverRequiresReconnect call sees empty lists and cannot push_back
    // into a list that is already being destroyed.
    std::list<std::shared_ptr<fss_server>> all;
    all.splice(all.end(), this->servers);
    all.splice(all.end(), this->reconnect_servers);
    for (const auto &server : all)
    {
        server->disconnect();
    }
}

void flight_safety_system::client_ssl::fss_client::disconnect()
{
    for (const auto &server : this->servers)
    {
        server->disconnect();
    }
}

void flight_safety_system::client_ssl::fss_client::setAssetName(std::string t_asset_name)
{
    this->asset_name = std::move(t_asset_name);
}

void flight_safety_system::client_ssl::fss_client::connectTo(const std::string &t_address, uint16_t t_port,
                                                             bool t_connect)
{
    auto server = std::make_shared<flight_safety_system::client_ssl::fss_server>(
        this, t_address, t_port, this->ca_file, this->private_key_file, this->public_key_file);
    if (t_connect)
    {
        server->reconnect();
    }
    this->addServer(server);
}

void flight_safety_system::client_ssl::fss_client::attemptReconnect()
{
    std::list<fss_server *> timed_out;
    for (const auto &server : this->servers)
    {
        if (server->isServerTimedOut())
        {
            timed_out.push_back(server.get());
        }
    }
    for (auto *server : timed_out)
    {
        FSS_LOG_WARN("client", "Server connection timed out, scheduling reconnect");
        this->serverRequiresReconnect(server);
    }

    std::list<std::shared_ptr<fss_server>> reconnected;
    bool any_connected = false;
    for (auto const &server : this->reconnect_servers)
    {
        if (server->reconnect())
        {
            reconnected.push_back(server);
            any_connected = true;
        }
    }
    while (!reconnected.empty())
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

void flight_safety_system::client_ssl::fss_client::sendMsgAll(
    const std::shared_ptr<flight_safety_system::transport::fss_message> &msg)
{
    for (auto const &server : this->servers)
    {
        server->sendMsg(msg);
    }
}

auto flight_safety_system::client_ssl::fss_client::getAssetName() -> std::string
{
    return this->asset_name;
}

void flight_safety_system::client_ssl::fss_client::addServer(
    const std::shared_ptr<flight_safety_system::client_ssl::fss_server> &server)
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

static auto server_list_matches(const std::list<std::shared_ptr<flight_safety_system::client_ssl::fss_server>> &servers,
                                const std::string &address, uint16_t port) -> bool
{
    return std::any_of(servers.begin(), servers.end(), [&address, &port](const auto &n) -> auto {
        return (n->getAddress().compare(address) == 0 && n->getPort() == port);
    });
}

void flight_safety_system::client_ssl::fss_client::updateServers(
    const std::shared_ptr<flight_safety_system::transport::fss_message_server_list> &msg)
{
    for (auto const &server_entry : msg->getServers())
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

void flight_safety_system::client_ssl::fss_client::serverRequiresReconnect(
    flight_safety_system::client_ssl::fss_server *server)
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

void flight_safety_system::client_ssl::fss_client::notifyConnectionStatus()
{
    switch (this->servers.size())
    {
        case 0: this->connectionStatusChange(CLIENT_CONNECTION_STATUS_DISCONNECTED); break;
        case 1: this->connectionStatusChange(CLIENT_CONNECTION_STATUS_CONNECTED_1_SERVER); break;
        default: this->connectionStatusChange(CLIENT_CONNECTION_STATUS_CONNECTED_2_OR_MORE); break;
    }
}

void flight_safety_system::client_ssl::fss_client::connectionStatusChange(
    flight_safety_system::client_ssl::connection_status status __attribute__((unused)))
{
}

void flight_safety_system::client_ssl::fss_client::handleCommand(
    const std::shared_ptr<flight_safety_system::transport::fss_message_asset_command> &msg __attribute__((unused)))
{
}

void flight_safety_system::client_ssl::fss_client::handlePositionReport(
    const std::shared_ptr<flight_safety_system::transport::fss_message_position_report> &msg __attribute__((unused)))
{
}

void flight_safety_system::client_ssl::fss_client::handleSMMSettings(
    const std::shared_ptr<flight_safety_system::transport::fss_message_smm_settings> &msg __attribute__((unused)))
{
}

flight_safety_system::client_ssl::fss_server::fss_server(flight_safety_system::client_ssl::fss_client *t_client,
                                                         std::string t_address, uint16_t t_port, std::string t_ca,
                                                         std::string t_private_key, std::string t_public_key)
    : flight_safety_system::transport::fss_message_cb(nullptr), client(t_client), address(std::move(t_address)),
      port(t_port), ca_file(std::move(t_ca)), private_key_file(std::move(t_private_key)),
      public_key_file(std::move(t_public_key))
{
}

flight_safety_system::client_ssl::fss_server::~fss_server() = default;

auto flight_safety_system::client_ssl::fss_server::getAddress() -> std::string
{
    return this->address;
}

auto flight_safety_system::client_ssl::fss_server::getPort() -> uint16_t
{
    return this->port;
}

auto flight_safety_system::client_ssl::fss_server::getClient() -> fss_client *
{
    return this->client;
}


void flight_safety_system::client_ssl::fss_server::sendIdentify()
{
    auto ident_msg =
        std::make_shared<flight_safety_system::transport::fss_message_identity>(this->client->getAssetName());
    this->getConnection()->sendMsg(ident_msg);
}

void flight_safety_system::client_ssl::fss_server::sendVersion()
{
    auto version_msg = std::make_shared<flight_safety_system::transport::fss_message_version>();
    this->getConnection()->sendMsg(version_msg);
}

auto flight_safety_system::client_ssl::fss_server::reconnect_to() -> bool
{
    auto new_conn = flight_safety_system::transport_ssl::fss_connection_client::create(
        this->ca_file, this->private_key_file, this->public_key_file, this->getAddress(), this->getPort());
    if (new_conn == nullptr)
    {
        return false;
    }
    this->setConnection(new_conn);
    return true;
}

void flight_safety_system::client_ssl::fss_server::setClock(std::shared_ptr<flight_safety_system::IClock> t_clock)
{
    if (t_clock == nullptr)
    {
        return;
    }
    this->clock = std::move(t_clock);
}

auto flight_safety_system::client_ssl::fss_server::reconnect() -> bool
{
    uint64_t ts = this->clock->now_ms();
    uint64_t elapsed_time = ts - this->last_tried;

    if (this->getConnection() != nullptr)
    {
        this->clearConnection();
    }

    if (elapsed_time > this->effective_delay)
    {
        this->retry_count++;
        if (this->retry_delay < retry_delay_cap)
        {
            this->retry_delay += this->retry_delay;
        }
        int64_t jitter_range = static_cast<int64_t>(this->retry_delay) / 4;
        std::uniform_int_distribution<int64_t> dist(-jitter_range, jitter_range);
        this->effective_delay = static_cast<uint64_t>(static_cast<int64_t>(this->retry_delay) + dist(this->rng));
        this->last_tried = ts;
        if (!this->reconnect_to())
        {
            this->clearConnection();
        }
        else
        {
            this->getConnection()->setHandler(this);
            /* Protocol version handshake must be the first message
             * exchanged after TLS connect, before identity. */
            this->sendVersion();
            this->sendIdentify();
            this->retry_count = 0;
            this->last_tried = 0;
            this->retry_delay = retry_delay_start;
            this->effective_delay = retry_delay_start;
            return true;
        }
    }
    return false;
}

auto flight_safety_system::client_ssl::fss_server::isServerTimedOut() -> bool
{
    if (!this->liveness_active.load(std::memory_order_relaxed))
    {
        return false;
    }
    return (this->clock->now_ms() - this->last_message_received_time.load(std::memory_order_relaxed)) >
           this->server_timeout_ms;
}

void flight_safety_system::client_ssl::fss_server::processMessage(
    std::shared_ptr<flight_safety_system::transport::fss_message> msg)
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
        this->liveness_active.store(true, std::memory_order_relaxed);
        this->last_message_received_time.store(this->clock->now_ms(), std::memory_order_relaxed);
        switch (msg->getType())
        {
            case flight_safety_system::transport::message_type_unknown:
            case flight_safety_system::transport::message_type_closed:
            case flight_safety_system::transport::message_type_identity:
            case flight_safety_system::transport::message_type_identity_non_aircraft:
            case flight_safety_system::transport::message_type_identity_required: break;
            case flight_safety_system::transport::message_type_version: {
                auto version_msg = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_version>(msg);
                if (version_msg != nullptr)
                {
                    uint16_t peer_version = version_msg->getProtocolVersion();
                    uint16_t peer_min = version_msg->getMinSupportedVersion();
                    if (peer_version < flight_safety_system::transport::FSS_PROTOCOL_MIN_VERSION ||
                        peer_min > flight_safety_system::transport::FSS_PROTOCOL_VERSION)
                    {
                        FSS_LOG_ERROR("client",
                                      "Server protocol version incompatible: peer="
                                          << peer_version << " peer_min=" << peer_min
                                          << " us=" << flight_safety_system::transport::FSS_PROTOCOL_VERSION
                                          << " us_min=" << flight_safety_system::transport::FSS_PROTOCOL_MIN_VERSION);
                        this->getClient()->serverRequiresReconnect(this);
                        return;
                    }
                    uint16_t negotiated = std::min(peer_version, flight_safety_system::transport::FSS_PROTOCOL_VERSION);
                    this->getConnection()->setNegotiatedVersion(negotiated);
                }
            }
            break;
            case flight_safety_system::transport::message_type_rtt_request: {
                /* Send a response */
                auto reply_msg =
                    std::make_shared<flight_safety_system::transport::fss_message_rtt_response>(msg->getId());
                this->sendMsg(reply_msg);
            }
            break;
            case flight_safety_system::transport::message_type_rtt_response:
                /* Currently we don't send any rtt requests */
                break;
            case flight_safety_system::transport::message_type_position_report: {
                /* Servers will be relaying position reports, so this is another asset */
                auto position_msg =
                    std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_position_report>(msg);
                if (position_msg != nullptr)
                {
                    this->getClient()->handlePositionReport(position_msg);
                }
            }
            break;
            case flight_safety_system::transport::message_type_system_status:
            /* Servers don't currently send status reports */
            case flight_safety_system::transport::message_type_search_status:
                /* Servers don't have a search status to report */
                break;
            case flight_safety_system::transport::message_type_command: {
                auto command_msg =
                    std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_asset_command>(msg);
                if (command_msg != nullptr)
                {
                    this->getClient()->handleCommand(command_msg);
                }
            }
            break;
            case flight_safety_system::transport::message_type_server_list: {
                auto server_list_msg =
                    std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_server_list>(msg);
                if (server_list_msg != nullptr)
                {
                    this->getClient()->updateServers(server_list_msg);
                }
            }
            break;
            case flight_safety_system::transport::message_type_smm_settings: {
                auto smm_settings_msg =
                    std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_smm_settings>(msg);
                if (smm_settings_msg != nullptr)
                {
                    this->getClient()->handleSMMSettings(smm_settings_msg);
                }
            }
            break;
        }
    }
}
