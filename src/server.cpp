#include "fss-transport.hpp"
#include "fss.hpp"
#include "fss-server.hpp"

#include <iostream>
#include <fstream>
#include <csignal>
#include <list>
#include <mutex>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
#include <json/json.h>
#pragma GCC diagnostic pop

#include <unistd.h>

using namespace flight_safety_system::server;
namespace fss_transport = flight_safety_system::transport;

db_connection *dbc = nullptr;

class server_clients{
private:
    std::mutex lock{};
    std::list<fss_client *> clients{};
    std::queue<fss_client *> disconnected{};
    uint32_t total_clients{0};
public:
    server_clients() {};
    ~server_clients() {
        std::queue<fss_client *> toDisconnect;
        /* Prevent changes while we empty the client list */
        this->lock.lock();
        for (auto c: this->clients)
        {
            toDisconnect.push(c);
        }
        this->clients.clear();
        this->lock.unlock();
        /* Disconnect the clients that were still in the list */
        while (!toDisconnect.empty())
        {
            auto c = toDisconnect.front();
            toDisconnect.pop();
            c->disconnect();
        }
        /* Wait for all clients to move to disconnected state */
        while (total_clients > this->disconnected.size())
        {
            this->lock.unlock();
            sleep (1);
            this->lock.lock();
        }
        this->lock.unlock();
        this->cleanupRemovableClients();
    }
    void cleanupRemovableClients()
    {
        this->lock.lock();
        while(!this->disconnected.empty())
        {
            fss_client *client = this->disconnected.front();
            this->disconnected.pop();
            total_clients--;
            delete client;
        }
        this->lock.unlock();
    };
    void clientConnected(fss_client *client)
    {
        this->lock.lock();
        this->total_clients++;
        this->clients.push_back(client);
        this->lock.unlock();
    };
    void clientDisconnected(fss_client *client)
    {
        this->lock.lock();
        this->clients.remove(client);
        this->disconnected.push(client);
        this->lock.unlock();
    };
    void sendMsg(fss_transport::fss_message *msg, fss_client *except = nullptr)
    {
        for (auto client : clients)
        {
            if (client->isAircraft() && client != except)
            {
                client->sendMsg(msg);
            }
        }
    }
    void sendSMMSettings()
    {
        for(auto client: clients)
        {
            client->sendSMMSettings();
        }
    };
    void sendRTTRequest(fss_transport::fss_message_rtt_request *rtt_req)
    {
        for(auto client: this->clients)
        {
            client->sendRTTRequest(rtt_req);
        }
    };
    void sendCommand()
    {
        for(auto client: this->clients)
        {
            client->sendCommand();
        }
    };
};

server_clients *clients = nullptr;

fss_client::fss_client(fss_transport::fss_connection *t_conn) : fss_message_cb(t_conn)
{
    conn->setHandler(this);
}

fss_client::~fss_client()
{
    if (this->conn != nullptr)
    {
        delete this->conn;
        this->conn = nullptr;
    }
    for(auto rtt_req : this->outstanding_rtt_requests)
    {
        delete rtt_req;
    }
    this->outstanding_rtt_requests.clear();
}

void
fss_client::sendMsg(fss_transport::fss_message *msg)
{
    this->conn->sendMsg(msg);
}

void
fss_client::sendCommand()
{
    uint64_t ts = fss_current_timestamp();
    auto ac = dbc->asset_get_command(this->name);
    if (ac != nullptr && (ac->getDBId() != this->last_command_dbid || ts > (this->last_command_send_ts + 10000)))
    {
        /* New command or time to re-send */
        this->last_command_send_ts = ts;
        this->last_command_dbid = ac->getDBId();
        fss_transport::fss_message_asset_command *msg = nullptr;
        auto command = ac->getCommand();
        switch (command)
        {
            case fss_transport::asset_command_goto:
                msg = new fss_transport::fss_message_asset_command(command, ac->getTimeStamp(), ac->getLatitude(), ac->getLongitude());
                break;
            case fss_transport::asset_command_altitude:
                msg = new fss_transport::fss_message_asset_command(command, ac->getTimeStamp(), ac->getAltitude());
                break;
            default:
                msg = new fss_transport::fss_message_asset_command(command, ac->getTimeStamp());
                break;
        }
        this->conn->sendMsg(msg);
        delete msg;
    }
    delete ac;
}

void
fss_client::disconnect()
{
    this->conn->disconnect();
    delete this->conn;
    this->conn = nullptr;
}

void
fss_client::sendRTTRequest(fss_transport::fss_message_rtt_request *rtt_req)
{
    uint64_t ts = fss_current_timestamp();
    this->conn->sendMsg(rtt_req);
    this->outstanding_rtt_requests.push_back(new fss_client_rtt(ts, rtt_req->getId()));
}

void
fss_client::sendSMMSettings()
{
    smm_settings *smm = dbc->asset_get_smm_settings(this->name);
    if (smm != nullptr)
    {
        auto settings_msg = new fss_transport::fss_message_smm_settings(smm->getAddress(), smm->getUsername(), smm->getPassword());
        this->conn->sendMsg(settings_msg);
        delete settings_msg;
    }
    delete smm;
}

void
fss_client::processMessage(fss_transport::fss_message *msg)
{
#ifdef DEBUG
    std::cout << "Got message " << msg->getType() << std::endl;
#endif
    if (msg->getType() == fss_transport::message_type_closed)
    {
        /* Connection has been closed, cleanup */
        clients->clientDisconnected(this);
        return;
    }
    if (!this->identified)
    {
        /* Only accept identify messages */
        if (msg->getType() == fss_transport::message_type_identity)
        {
            this->name = ((fss_transport::fss_message_identity *)msg)->getName();
            this->identified = true;
            this->aircraft = true;
            /* send the current command */
            this->sendCommand();
            /* send SMM config and servers list */
            this->sendSMMSettings();
            /* Send all the known fss servers */
            auto known_servers = dbc->get_active_fss_servers();
            auto server_list = new fss_transport::fss_message_server_list();
            for (auto server_details : known_servers)
            {
                server_list->addServer(server_details->getAddress(),server_details->getPort());
                delete server_details;
            }
            this->conn->sendMsg(server_list);
            delete server_list;
        }
        else if(msg->getType() == fss_transport::message_type_identity_non_aircraft)
        {
            this->identified = true;
            this->aircraft = false;
        }
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
            {
                /* Find the original message and calculate the response time */
                fss_client_rtt *rtt_req = nullptr;
                uint64_t current_ts = fss_current_timestamp();
                for(auto req : this->outstanding_rtt_requests)
                {
                    if(req->getRequestId() == ((fss_transport::fss_message_rtt_response *)msg)->getRequestId())
                    {
                        rtt_req = req;
                    }
                }
                if (rtt_req)
                {
                    this->outstanding_rtt_requests.remove(rtt_req);
#ifdef DEBUG
                    std::cout << "RTT for " << this->getName() << " is " << (current_ts - rtt_req->getTimeStamp()) << std::endl;
#endif
                    dbc->asset_add_rtt(this->name, current_ts - rtt_req->getTimeStamp());
                    delete rtt_req;
                }
            }
                break;
            case fss_transport::message_type_position_report:
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
            case fss_transport::message_type_system_status:
            {
                /* Capture and store in the database */
                auto status_msg = (fss_transport::fss_message_system_status *)msg;
                dbc->asset_add_status(this->name, status_msg->getBatRemaining(), status_msg->getBatMAHUsed());
            }
                break;
            case fss_transport::message_type_search_status:
            {
                /* Capture and store in the database */
                auto status_msg = (fss_transport::fss_message_search_status *)msg;
                dbc->asset_add_search_status(this->name, status_msg->getSearchId(), status_msg->getSearchCompleted(), status_msg->getSearchTotal());
            }
                break;
            /* These are server->client only */
            case fss_transport::message_type_command:
            case fss_transport::message_type_server_list:
            case fss_transport::message_type_smm_settings:
                break;
        }
    }
}

bool running = true;

void sigIntHandler(int signum)
{
    running = false;
}

bool new_client_connect(fss_transport::fss_connection *conn)
{
#ifdef DEBUG
    std::cout << "New client connected" << std::endl;
#endif
    clients->clientConnected(new fss_client(conn));
    return true;
}

int main(int argc, char *argv[])
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
    dbc = new db_connection(config["postgres"]["host"].asString(), config["postgres"]["user"].asString(), config["postgres"]["pass"].asString(), config["postgres"]["db"].asString());
    
    /* Create the clients tracking */
    clients = new server_clients();

    /* Open listen socket */
    auto listen = new fss_transport::fss_listen(config["port"].asInt(), new_client_connect);
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
    while (running)
    {
        sleep (1);
        clients->cleanupRemovableClients();
        /* Send RTT messages to all clients */
        {
            auto rtt_req = new fss_transport::fss_message_rtt_request();
            clients->sendRTTRequest(rtt_req);
            clients->sendCommand();
            delete rtt_req;
        }
        /* Send Config settings to all clients */
        if ((counter % 15) == 0)
        {
            /* Send all the known fss servers */
            auto known_servers = dbc->get_active_fss_servers();
            auto server_list = new fss_transport::fss_message_server_list();
            for (auto server_details : known_servers)
            {
                server_list->addServer(server_details->getAddress(),server_details->getPort());
                delete server_details;
            }
            clients->sendMsg(server_list);
            delete server_list;
            clients->sendSMMSettings();
        }
        counter++;
    }
    
    /* Close the listen socket */
    delete listen;
    listen = nullptr;
    /* Disconnect all the clients */
    delete clients;
    clients = nullptr;
    /* Disconnect the database */
    delete dbc;
    dbc = nullptr;
}
