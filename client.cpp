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

std::list<fss_connection *> servers;

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
        servers.push_back(conn);
    }

    /* Send each server some fake details */
    for (auto server: servers)
    {
        fss_message_system_status *msg_status = new fss_message_system_status(server->getMessageId(), 75, 1000);
        fss_message_search_status *msg_search = new fss_message_search_status(server->getMessageId(), 1, 23, 100);
        fss_message_position_report *msg_pos = new fss_message_position_report(server->getMessageId(), -43.5, 172.5, 300, fss_current_timestamp());
        server->sendMsg(msg_status);
        server->sendMsg(msg_search);
        server->sendMsg(msg_pos);
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
