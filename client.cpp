#include "fss-internal.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <list>
#include <csignal>

#include <json/json.h>

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
