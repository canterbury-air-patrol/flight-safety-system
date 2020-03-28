#include "fss.hpp"
#include "fss-server.hpp"

#include <iostream>
#include <fstream>
#include <csignal>
#include <list>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
#include <json/json.h>
#pragma GCC diagnostic pop

#include <unistd.h>

using namespace flight_safety_system::server;
namespace fss_transport = flight_safety_system::transport;

db_connection *dbc = nullptr;

std::list<fss_client *> clients;
std::queue<fss_client *> remove_clients;

fss_client::fss_client(fss_transport::fss_connection *t_conn) : fss_message_cb(t_conn), identified(false), name(), outstanding_rtt_requests(), last_command_send_ts(0), last_command_dbid(0)
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
        clients.remove(this);
        remove_clients.push(this);
        return;
    }
    if (!this->identified)
    {
        /* Only accept identify messages */
        if (msg->getType() == fss_transport::message_type_identity)
        {
            this->name = ((fss_transport::fss_message_identity *)msg)->getName();
            this->identified = true;
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
                /* Capture and store in the database */
                dbc->asset_add_position(this->name, msg->getLatitude(), msg->getLongitude(), msg->getAltitude());
                /* Ideally reflect this message to all clients */
                
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
    clients.push_back(new fss_client(conn));
    return true;
}

void cleanup_removable_clients()
{
    while(!remove_clients.empty())
    {
        fss_client *client = remove_clients.front();
        remove_clients.pop();
        delete client;
    }
}

int main(int argc, char *argv[])
{
    /* Watch out for sigint */
    signal (SIGINT, sigIntHandler);
    /* Ignore sig pipe */
    signal (SIGPIPE, SIG_IGN);
    /* Read config */
    std::ifstream configfile((argc > 1 ? std::string(argv[1]) : "/etc/fss/server.json"));
    if (!configfile.is_open())
    {
        printf("Failed to load configuration\n");
        exit(-1);
    }
    Json::Value config;
    configfile >> config;

    /* Connect to database */
    dbc = new db_connection(config["postgres"]["host"].asString(), config["postgres"]["user"].asString(), config["postgres"]["pass"].asString(), config["postgres"]["db"].asString());
    
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
        cleanup_removable_clients();
        /* Send RTT messages to all clients */
        auto rtt_req = new fss_transport::fss_message_rtt_request();
        for(auto client: clients)
        {
            client->sendRTTRequest(rtt_req);
            client->sendCommand();
        }
        delete rtt_req;
        /* Send Config settings to all clients */
        if (counter == 30)
        {
            /* Send all the known fss servers */
            auto known_servers = dbc->get_active_fss_servers();
            auto server_list = new fss_transport::fss_message_server_list();
            for (auto server_details : known_servers)
            {
                server_list->addServer(server_details->getAddress(),server_details->getPort());
                delete server_details;
            }
            for(auto client: clients)
            {
                client->sendMsg(server_list);
            }
            delete server_list;
            for(auto client: clients)
            {
                client->sendSMMSettings();
            }
        }
        counter++;
    }
    
    delete listen;
    listen = nullptr;
    /* Disconnect all the clients */
    while (!clients.empty())
    {
        fss_client *client = clients.front();
        clients.pop_front();
        delete client;
    }
    cleanup_removable_clients();
    delete dbc;
    dbc = nullptr;
}