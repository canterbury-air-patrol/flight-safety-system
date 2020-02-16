#include "fss-internal.hpp"
#include "fss-server.hpp"

#include <iostream>
#include <fstream>
#include <csignal>
#include <list>

#include <json/json.h>

#include <unistd.h>

bool running = true;

db_connection *dbc = NULL;

void sigIntHandler(int signum)
{
    running = false;
}

bool client_message(fss_connection *conn, fss_message *message)
{
    return true;
}

std::list<fss_connection *> client_connections;

bool new_client_connect(fss_connection *conn)
{
    std::cout << "New client connected" << std::endl;
    conn->setHandler(client_message);
    client_connections.push_back(conn);
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
    }
    
    delete dbc;
    dbc = NULL;
    delete listen;
    listen = NULL;
}
