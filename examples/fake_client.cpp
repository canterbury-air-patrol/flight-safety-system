#include "fss-client.hpp"

#include <csignal>

#include <unistd.h>

bool running = true;

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

    auto client = new flight_safety_system::client::fss_client(argv[1]);
    
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
    
    int counter = 0;
    while (running)
    {
        sleep (1);
        client->attemptReconnect();
        counter++;
        if (counter % 5 == 0)
        {
            /* Send each server some fake details */
            auto msg_status = new flight_safety_system::transport::fss_message_system_status(75, 1000);
            auto msg_search = new flight_safety_system::transport::fss_message_search_status(1, 23, 100);
            auto msg_pos = new flight_safety_system::transport::fss_message_position_report(-43.5, 172.5, 300, flight_safety_system::fss_current_timestamp());
            client->sendMsgAll(msg_status);
            client->sendMsgAll(msg_search);
            client->sendMsgAll(msg_pos);
            delete msg_status;
            delete msg_search;
            delete msg_pos;
        }
    }
}