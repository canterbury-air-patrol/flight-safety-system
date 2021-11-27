#include "fss-client-ssl.hpp"

#include <csignal>

#include <unistd.h>

bool running = true;

void sigIntHandler(int signum __attribute__((unused)))
{
    running = false;
}

auto
main(int argc, char *argv[]) -> int
{
    if (argc < 2)
    {
        std::cout << "Insufficient arguments" << std::endl;
        return -1;
    }
    /* Watch out for sigint */
    signal (SIGINT, sigIntHandler);

    auto client = std::make_shared<flight_safety_system::client_ssl::fss_client>(argv[1]);

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
    
    constexpr int send_interval = 5;
    int counter = 0;
    while (running)
    {
        sleep (1);
        client->attemptReconnect();
        counter++;
        if (counter % send_interval == 0)
        {
            /* Send each server some fake details */
            constexpr int bat_remaining = 75;
            constexpr int bat_mah_used = 1000;
            constexpr double bat_voltage = 11.4;
            auto msg_status = std::make_shared<flight_safety_system::transport::fss_message_system_status>(bat_remaining, bat_mah_used, bat_voltage);
            constexpr int search_number = 1;
            constexpr int search_current_point = 23;
            constexpr int search_total_points = 100;
            auto msg_search = std::make_shared<flight_safety_system::transport::fss_message_search_status>(search_number, search_current_point, search_total_points);
            constexpr double lat = -43.5;
            constexpr double lng = 172.5;
            constexpr int alt = 300;
            constexpr int heading_cdeg = 1800;
            constexpr int hor_vel = 200;
            constexpr int vert_vel = 0;
            constexpr int icao_address = 0;
            constexpr const char *callsign = "example";
            constexpr int squawk_code = 01200;
            constexpr int time_since_last_contact = 0;
            constexpr int flags = 1 | 2 | 4 | 8 | 16 | 32;
            constexpr int alt_type = 1;
            constexpr int emitter_type = 14;
            auto msg_pos = std::make_shared<flight_safety_system::transport::fss_message_position_report>(lat, lng, alt, heading_cdeg, hor_vel, vert_vel, icao_address, callsign, squawk_code, time_since_last_contact, flags, alt_type, emitter_type, flight_safety_system::fss_current_timestamp());
            client->sendMsgAll(msg_status);
            client->sendMsgAll(msg_search);
            client->sendMsgAll(msg_pos);
        }
    }
    client->disconnect();
}
