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

bool running = true;

class fss_server: public fss_message_cb {
private:
    std::string address;
    uint16_t port;
public:
    explicit fss_server(fss_connection *t_conn, std::string t_address, uint16_t t_port);
    virtual ~fss_server();
    virtual void processMessage(fss_message *message) override;
    std::string getAddress() { return this->address; };
    uint16_t getPort() { return this->port; };
};

std::list<fss_server *> servers;
std::list<fss_server *> reconnect_servers;

fss_server::fss_server(fss_connection *t_conn, std::string t_address, uint16_t t_port) : fss_message_cb(t_conn), address(t_address), port(t_port)
{
    conn->setHandler(this);
}

fss_server::~fss_server()
{
    if (this->conn != NULL)
    {
        delete this->conn;
        this->conn = NULL;
    }
}

void
fss_server::processMessage(fss_message *msg)
{
#ifdef DEBUG
    std::cout << "Got message " << msg->getType() << std::endl;
#endif
    if (msg->getType() == message_type_closed)
    {
        /* Connection has been closed, schedule reconnection */
        servers.remove(this);
        reconnect_servers.push_back(this);
        return;
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
            /* Currently we don't send any rtt requests */
                break;
            case message_type_position_report:
            /* Servers will be relaying position reports, so this is another asset */
                break;
            case message_type_system_status:
            /* Servers don't currently send status reports */
                break;
            case message_type_search_status:
            /* Servers don't have a search status to report */
                break;
            case message_type_command:
            {
                /* TODO */
            }
                break;
            case message_type_server_list:
            {
                /* TODO */
            }
                break;
            case message_type_smm_settings:
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

    fss connection = fss(config["name"].asString());

    for (unsigned int idx = 0; idx < config["servers"].size(); idx++)
    {
        fss_connection *conn = connection.connect(config["servers"][idx]["address"].asString(), config["servers"][idx]["port"].asInt());
        if (conn == NULL)
        {
            return -1;
        }
        servers.push_back(new fss_server(conn, config["servers"][idx]["address"].asString(), config["servers"][idx]["port"].asInt()));
    }

    /* Send each server some fake details */
    for (auto server: servers)
    {
        fss_message_system_status *msg_status = new fss_message_system_status(server->getConnection()->getMessageId(), 75, 1000);
        fss_message_search_status *msg_search = new fss_message_search_status(server->getConnection()->getMessageId(), 1, 23, 100);
        fss_message_position_report *msg_pos = new fss_message_position_report(server->getConnection()->getMessageId(), -43.5, 172.5, 300, fss_current_timestamp());
        server->getConnection()->sendMsg(msg_status);
        server->getConnection()->sendMsg(msg_search);
        server->getConnection()->sendMsg(msg_pos);
        delete msg_status;
        delete msg_search;
        delete msg_pos;
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
    
    while (running)
    {
        sleep (1);
    }
    
    for (auto conn : servers)
    {
        delete conn;
    }
}
