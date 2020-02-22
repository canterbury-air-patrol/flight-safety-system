#include "fss-internal.hpp"
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

db_connection *dbc = NULL;

std::list<fss_client *> clients;
std::queue<fss_client *> remove_clients;

fss_client::fss_client(fss_connection *t_conn) : fss_message_cb(t_conn), identified(false), name(), outstanding_rtt_requests()
{
    conn->setHandler(this);
}

fss_client::~fss_client()
{
    if (this->conn != NULL)
    {
        delete this->conn;
        this->conn = NULL;
    }
    for(auto rtt_req : this->outstanding_rtt_requests)
    {
        delete rtt_req;
    }
    this->outstanding_rtt_requests.clear();
}

void
fss_client::processMessage(fss_message *msg)
{
#ifdef DEBUG
    std::cout << "Got message " << msg->getType() << std::endl;
#endif
    if (msg->getType() == message_type_closed)
    {
        /* Connection has been closed, cleanup */
        clients.remove(this);
        remove_clients.push(this);
        return;
    }
    if (!this->identified)
    {
        /* Only accept identify messages */
        if (msg->getType() == message_type_identity)
        {
            this->name = ((fss_message_identity *)msg)->getName();
            this->identified = true;
            // send SMM config and servers list
            smm_settings *smm = dbc->asset_get_smm_settings(this->name);
            if (smm != nullptr)
            {
                fss_message_smm_settings *settings_msg = new fss_message_smm_settings(this->conn->getMessageId(), smm->getAddress(), smm->getUsername(), smm->getPassword());
                this->conn->sendMsg(settings_msg);
                delete settings_msg;
            }
            delete smm;
        }
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
                fss_message_rtt_response *reply_msg = new fss_message_rtt_response(conn->getMessageId(), msg->getId());
                conn->sendMsg(reply_msg);
                delete reply_msg;
            }
                break;
            case message_type_rtt_response:
            {
                /* Find the original message and calculate the response time */
                fss_client_rtt *rtt_req = NULL;
                uint64_t current_ts = fss_current_timestamp();
                for(fss_client_rtt *req : this->outstanding_rtt_requests)
                {
                    if(req->getRequestId() == ((fss_message_rtt_response *)msg)->getRequestId())
                    {
                        rtt_req = req;
                    }
                }
                if (rtt_req)
                {
                    this->outstanding_rtt_requests.remove(rtt_req);
                    dbc->asset_add_rtt(this->name, current_ts - rtt_req->getTimeStamp());
                    delete rtt_req;
                }
            }
                break;
            case message_type_position_report:
            {
                /* Capture and store in the database */
                dbc->asset_add_position(this->name, msg->getLatitude(), msg->getLongitude(), msg->getAltitude());
                /* Ideally reflect this message to all clients */
                
            }
                break;
            case message_type_system_status:
            {
                /* Capture and store in the database */
                fss_message_system_status *status_msg = (fss_message_system_status *)msg;
                dbc->asset_add_status(this->name, status_msg->getBatRemaining(), status_msg->getBatMAHUsed());
            }
                break;
            case message_type_search_status:
            {
                /* Capture and store in the database */
                fss_message_search_status *status_msg = (fss_message_search_status *)msg;
                dbc->asset_add_search_status(this->name, status_msg->getSearchId(), status_msg->getSearchCompleted(), status_msg->getSearchTotal());
            }
                break;
            /* These are server->client only */
            case message_type_command:
            case message_type_server_list:
            case message_type_smm_settings:
                break;
        }
    }
}

bool running = true;

void sigIntHandler(int signum)
{
    running = false;
}

bool new_client_connect(fss_connection *conn)
{
#ifdef DEBUG
    std::cout << "New client connected" << std::endl;
#endif
    clients.push_back(new fss_client(conn));
    return true;
}

int main(int argc, char *argv[])
{
    /* Watch out for sigint */
    signal (SIGINT, sigIntHandler);
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
    fss_listen *listen = new fss_listen(config["port"].asInt(), new_client_connect);
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

    while (running)
    {
        sleep (1);
        while(!remove_clients.empty())
        {
            fss_client *client = remove_clients.front();
            remove_clients.pop();
            delete client;
        }
    }
    
    delete dbc;
    dbc = NULL;
    delete listen;
    listen = NULL;
}
