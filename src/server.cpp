#include "fss-transport.hpp"
#include <ostream>
#include "fss-transport-ssl.hpp"
#include "fss.hpp"
#include "fss-server.hpp"

#include <iostream>
#include <fstream>
#include <csignal>
#include <list>
#include <memory>
#include <mutex>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
#include <json/json.h>
#pragma GCC diagnostic pop

#include <unistd.h>

constexpr int sec_to_msec = 1000;

std::shared_ptr<flight_safety_system::server::db_connection> dbc = nullptr;

flight_safety_system::server::smm_settings::smm_settings(std::string t_address, std::string t_username, std::string t_password) : address(std::move(t_address)), username(std::move(t_username)), password(std::move(t_password))
{
}

auto
flight_safety_system::server::smm_settings::getAddress() -> std::string
{
    return this->address;
}
auto
flight_safety_system::server::smm_settings::getUsername() -> std::string
{
    return this->username;
}
auto
flight_safety_system::server::smm_settings::getPassword() -> std::string
{
    return this->password;
}

flight_safety_system::server::fss_server_details::fss_server_details(std::string t_address, uint16_t t_port) : address(std::move(t_address)), port(t_port)
{
}

auto
flight_safety_system::server::fss_server_details::getAddress() -> std::string
{
    return this->address;
}
auto
flight_safety_system::server::fss_server_details::getPort() -> uint16_t
{
    return this->port;
}

flight_safety_system::server::asset_command::asset_command(uint64_t t_dbid, uint64_t t_timestamp, const std::string &t_cmd, double t_latitude, double t_longitude, uint16_t t_altitude) : dbid(t_dbid), timestamp(t_timestamp), command(transport::asset_command_unknown), latitude(t_latitude), longitude(t_longitude), altitude(t_altitude)
{
    if (t_cmd == "RTL") {
        this->command = transport::asset_command_rtl;
    } else if (t_cmd == "HOLD") {
        this->command = transport::asset_command_hold;
    } else if (t_cmd == "GOTO") {
        this->command = transport::asset_command_goto;
    } else if (t_cmd == "RON") {
        this->command = transport::asset_command_resume;
    } else if (t_cmd == "DISARM") {
        this->command = transport::asset_command_disarm;
    } else if (t_cmd == "ALT") {
        this->command = transport::asset_command_altitude;
    } else if (t_cmd == "TERM") {
        this->command = transport::asset_command_terminate;
    } else if (t_cmd == "MAN") {
        this->command = transport::asset_command_manual;
    }
}

auto
flight_safety_system::server::asset_command::getDBId() -> uint64_t
{
    return this->dbid;
}
auto
flight_safety_system::server::asset_command::getTimeStamp() -> uint64_t
{
    return this->timestamp;
}
auto
flight_safety_system::server::asset_command::getCommand() -> transport::fss_asset_command
{
    return this->command;
}
auto
flight_safety_system::server::asset_command::getLatitude() -> double
{
    return this->latitude;
}
auto
flight_safety_system::server::asset_command::getLongitude() -> double
{
    return this->longitude;
}
auto
flight_safety_system::server::asset_command::getAltitude() -> uint16_t
{
    return this->altitude;
}

flight_safety_system::server::fss_client_rtt::fss_client_rtt(uint64_t t_timestamp, uint64_t t_reqid) : timestamp(t_timestamp), reqid(t_reqid)
{
}

auto
flight_safety_system::server::fss_client_rtt::getTimeStamp() -> uint64_t
{
    return this->timestamp;
}
auto
flight_safety_system::server::fss_client_rtt::getRequestId() -> uint64_t
{
    return this->reqid;
}

class server_clients{
private:
    std::mutex lock{};
    std::list<std::shared_ptr<flight_safety_system::server::fss_client>> clients{};
    std::queue<std::shared_ptr<flight_safety_system::server::fss_client>> disconnected{};
    uint32_t total_clients{0};
    bool shutting_down{false};
public:
    server_clients() = default;
    ~server_clients() {
        /* Prevent changes while we empty the client list */
        this->shutting_down = true;
        this->lock.lock();
        for (const auto &c: this->clients)
        {
            c->disconnect();
        }
        this->lock.unlock();
    }
    server_clients(server_clients&) = delete;
    server_clients(server_clients&&) = delete;
    auto operator=(server_clients&) -> server_clients& = delete;
    auto operator=(server_clients&&) -> server_clients& = delete;
    void cleanupRemovableClients()
    {
        this->lock.lock();
        while(!this->disconnected.empty())
        {
            auto client = this->disconnected.front();
            this->disconnected.pop();
            total_clients--;
        }
        this->lock.unlock();
    };
    void clientConnected(std::shared_ptr<flight_safety_system::server::fss_client> client)
    {
        this->lock.lock();
        this->total_clients++;
        this->clients.push_back(std::move(client));
        this->lock.unlock();
    };
    void clientDisconnected(flight_safety_system::server::fss_client *client)
    {
        /* If we are shutting down, don't worry */
        if (this->shutting_down) return;
        this->lock.lock();
        for (const auto &c : this->clients)
        {
            if (c.get() == client)
            {
                this->disconnected.push(c);
                this->clients.remove(c);
                break;
            }
        }
        this->lock.unlock();
    };
    void sendMsg(const std::shared_ptr<flight_safety_system::transport::fss_message> &msg, flight_safety_system::server::fss_client *except = nullptr)
    {
        for (const auto &client : clients)
        {
            if (client->isAircraft() && client.get() != except)
            {
                client->sendMsg(msg);
            }
        }
    }
    void sendSMMSettings()
    {
        for(const auto &client: clients)
        {
            client->sendSMMSettings();
        }
    };
    void sendRTTRequest(const std::shared_ptr<flight_safety_system::transport::fss_message_rtt_request> &rtt_req)
    {
        for(const auto &client: this->clients)
        {
            client->sendRTTRequest(rtt_req);
        }
    };
    void sendCommand()
    {
        for(const auto &client: this->clients)
        {
            client->sendCommand();
        }
    };
};

std::shared_ptr<server_clients> clients = nullptr;

flight_safety_system::server::fss_client::fss_client(std::shared_ptr<flight_safety_system::transport::fss_connection> t_conn) : fss_message_cb(std::move(t_conn))
{
    this->getConnection()->setHandler(this);
}

flight_safety_system::server::fss_client::~fss_client() = default;

void
flight_safety_system::server::fss_client::sendCommand()
{
    uint64_t ts = fss_current_timestamp();
    auto ac = dbc->asset_get_command(this->name);
    constexpr int timeout_time = 10 * sec_to_msec;
    if (ac != nullptr && (ac->getDBId() != this->last_command_dbid || ts > (this->last_command_send_ts + timeout_time)))
    {
        /* New command or time to re-send */
        this->last_command_send_ts = ts;
        this->last_command_dbid = ac->getDBId();
        std::shared_ptr<flight_safety_system::transport::fss_message_asset_command> msg = nullptr;
        auto command = ac->getCommand();
        switch (command)
        {
            case flight_safety_system::transport::asset_command_goto:
                msg = std::make_shared<flight_safety_system::transport::fss_message_asset_command>(command, ac->getTimeStamp(), ac->getLatitude(), ac->getLongitude());
                break;
            case flight_safety_system::transport::asset_command_altitude:
                msg = std::make_shared<flight_safety_system::transport::fss_message_asset_command>(command, ac->getTimeStamp(), ac->getAltitude());
                break;
            default:
                msg = std::make_shared<flight_safety_system::transport::fss_message_asset_command>(command, ac->getTimeStamp());
                break;
        }
        this->getConnection()->sendMsg(msg);
    }
}

auto
flight_safety_system::server::fss_client::isAircraft() -> bool
{
    return this->aircraft;
}

auto
flight_safety_system::server::fss_client::getName() -> std::string
{
    return this->name;
}


void
flight_safety_system::server::fss_client::sendRTTRequest(const std::shared_ptr<flight_safety_system::transport::fss_message_rtt_request> &rtt_req)
{
    uint64_t ts = fss_current_timestamp();
    this->getConnection()->sendMsg(rtt_req);
    this->outstanding_rtt_requests.push_back(std::make_shared<fss_client_rtt>(ts, rtt_req->getId()));
}

void
flight_safety_system::server::fss_client::sendSMMSettings()
{
    auto smm = dbc->asset_get_smm_settings(this->name);
    if (smm != nullptr)
    {
        auto settings_msg = std::make_shared<flight_safety_system::transport::fss_message_smm_settings>(smm->getAddress(), smm->getUsername(), smm->getPassword());
        this->getConnection()->sendMsg(settings_msg);
    }
}

void
flight_safety_system::server::fss_client::processMessage(std::shared_ptr<flight_safety_system::transport::fss_message> msg)
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
        /* Connection has been closed, cleanup */
        clients->clientDisconnected(this);
        return;
    }
    if (!this->identified)
    {
        /* Only accept identify messages */
        if (msg->getType() == flight_safety_system::transport::message_type_identity)
        {
            auto identity_msg = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_identity>(msg);
            if (identity_msg != nullptr)
            {
                this->name = identity_msg->getName();
                auto possible_names = this->getConnection()->getClientNames();
                if (possible_names.empty())
                {
                    /* No client names, so accept anything */
                    this->identified = true;
                }
                else
                {
                    for (const auto &possible_name: possible_names)
                    {
                        if (possible_name == this->name)
                        {
                            this->identified = true;
                            break;
                        }
                    }
                }
                if (!this->identified)
                {
                    clients->clientDisconnected(this);
                    return;
                }
                this->aircraft = true;
                /* send the current command */
                this->sendCommand();
                /* send SMM config and servers list */
                this->sendSMMSettings();
                /* Send all the known fss servers */
                auto known_servers = dbc->get_active_fss_servers();
                auto server_list = std::make_shared<flight_safety_system::transport::fss_message_server_list>();
                for (const auto &server_details : known_servers)
                {
                    server_list->addServer(server_details->getAddress(),server_details->getPort());
                }
                this->getConnection()->sendMsg(server_list);
            }
        }
        else if(msg->getType() == flight_safety_system::transport::message_type_identity_non_aircraft)
        {
            this->identified = true;
            this->aircraft = false;
        }
        else
        {
            this->sendMsg(std::make_shared<flight_safety_system::transport::fss_message_identity_required>());
        }
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
                this->getConnection()->sendMsg(reply_msg);
            }
                break;
            case flight_safety_system::transport::message_type_rtt_response:
            {
                /* Find the original message and calculate the response time */
                std::shared_ptr<fss_client_rtt> rtt_req = nullptr;
                uint64_t current_ts = fss_current_timestamp();
                auto rtt_resp_msg = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_rtt_response>(msg);
                if (rtt_resp_msg != nullptr)
                {
                    for(const auto &req : this->outstanding_rtt_requests)
                    {
                        if(req->getRequestId() == rtt_resp_msg->getRequestId())
                        {
                            rtt_req = req;
                        }
                    }
                }
                if (rtt_req != nullptr)
                {
                    this->outstanding_rtt_requests.remove(rtt_req);
#ifdef DEBUG
                    std::cout << "RTT for " << this->getName() << " is " << (current_ts - rtt_req->getTimeStamp()) << std::endl;
#endif
                    dbc->asset_add_rtt(this->name, current_ts - rtt_req->getTimeStamp());
                }
            }
                break;
            case flight_safety_system::transport::message_type_position_report:
            {
                if (this->aircraft)
                {
                    /* Capture and store in the database */
                    dbc->asset_add_position(this->name, msg->getLatitude(), msg->getLongitude(), msg->getAltitude());
                }
                /* Reflect this message to all aircraft clients */
                clients->sendMsg(msg, this);
            }
                break;
            case flight_safety_system::transport::message_type_system_status:
            {
                /* Capture and store in the database */
                auto status_msg = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_system_status>(msg);
                if (status_msg != nullptr)
                {
                    dbc->asset_add_status(this->name, status_msg->getBatRemaining(), status_msg->getBatMAHUsed(), status_msg->getBatVoltage());
                }
            }
                break;
            case flight_safety_system::transport::message_type_search_status:
            {
                /* Capture and store in the database */
                auto status_msg = std::dynamic_pointer_cast<flight_safety_system::transport::fss_message_search_status>(msg);
                if (status_msg != nullptr)
                {
                    dbc->asset_add_search_status(this->name, status_msg->getSearchId(), status_msg->getSearchCompleted(), status_msg->getSearchTotal());
                }
            }
                break;
            /* These are server->client only */
            case flight_safety_system::transport::message_type_command:
            case flight_safety_system::transport::message_type_server_list:
            case flight_safety_system::transport::message_type_smm_settings:
                break;
        }
    }
}

bool running = true;

void sigIntHandler(int signum __attribute__((unused)))
{
    running = false;
}

auto
new_client_connect(std::shared_ptr<flight_safety_system::transport::fss_connection> conn) -> bool
{
#ifdef DEBUG
    std::cout << "New client connected" << std::endl;
#endif
    clients->clientConnected(std::make_shared<flight_safety_system::server::fss_client>(std::move(conn)));
    return true;
}

auto
main(int argc, char *argv[]) -> int
{
    /* Watch out for sigint */
    signal (SIGINT, sigIntHandler);
    /* Ignore sig pipe */
    signal (SIGPIPE, SIG_IGN);
    /* Read config */
    std::string conf_file = (argc > 1 ? std::string(argv[1]) : "/etc/fss/server.json");
    std::ifstream configfile(conf_file);
    if (!configfile.is_open())
    {
        std::cerr << "Failed to load configuration: " << conf_file << std::endl;
        exit(-1);
    }
    Json::Value config;
    configfile >> config;

    /* Connect to database */
    dbc = std::make_shared<flight_safety_system::server::db_connection>(config["postgres"]["host"].asString(), config["postgres"]["user"].asString(), config["postgres"]["pass"].asString(), config["postgres"]["db"].asString());
    
    /* Create the clients tracking */
    clients = std::make_shared<server_clients>();

    /* Open listen socket */
    std::shared_ptr<flight_safety_system::transport::fss_listen> listen;
    std::cerr << "Starting fss server in TLS mode" << std::endl;
    std::string ca_public_key = config["ssl"]["ca_public_key"].asString();
    std::string server_private_key = config["ssl"]["server_private_key"].asString();
    std::string server_public_key = config["ssl"]["server_public_key"].asString();
    if (ca_public_key == "" || server_private_key == "" || server_public_key == "")
    {
        std::cerr << "Missing ssl parameter, all of these are required: 'ca_public_key', 'server_private_key', 'server_public_key'" << std::endl;
        exit(-1);
    }
    listen = std::make_shared<flight_safety_system::transport_ssl::fss_listen>(config["port"].asInt(), new_client_connect, config["ssl"]["ca_public_key"].asString(), config["ssl"]["server_private_key"].asString(), config["ssl"]["server_public_key"].asString());

    /* Process client messages:
       - Battery status
       - Position (reflect to other clients)
       - Search information
     */
    /* Events:
       - Client Connect
       - Client Disconnect
     */
    /* Periodic activity
       - Per client, send RTT message
     */

    int counter = 0;
    constexpr int send_config_period = 15;
    while (running)
    {
        sleep (1);
        clients->cleanupRemovableClients();
        /* Send RTT messages to all clients */
        {
            auto rtt_req = std::make_shared<flight_safety_system::transport::fss_message_rtt_request>();
            clients->sendRTTRequest(rtt_req);
            clients->sendCommand();
        }
        /* Send Config settings to all clients */
        if ((counter % send_config_period) == 0)
        {
            /* Send all the known fss servers */
            auto known_servers = dbc->get_active_fss_servers();
            auto server_list = std::make_shared<flight_safety_system::transport::fss_message_server_list>();
            for (const auto &server_details : known_servers)
            {
                server_list->addServer(server_details->getAddress(),server_details->getPort());
            }
            clients->sendMsg(server_list);
            clients->sendSMMSettings();
        }
        counter++;
    }
}
