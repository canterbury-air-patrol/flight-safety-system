#include "fss-client-ssl.hpp"

#include <csignal>
#include <iostream>
#include <string>

#include <unistd.h>

namespace fss = flight_safety_system;

volatile sig_atomic_t running = 1;

void sigIntHandler(int signum __attribute__((unused)))
{
    running = 0;
}

static auto commandName(fss::transport::fss_asset_command cmd) -> std::string
{
    switch (cmd)
    {
        case fss::transport::asset_command_rtl: return "RTL";
        case fss::transport::asset_command_hold: return "HOLD";
        case fss::transport::asset_command_goto: return "GOTO";
        case fss::transport::asset_command_resume: return "RON";
        case fss::transport::asset_command_terminate: return "TERM";
        case fss::transport::asset_command_disarm: return "DISARM";
        case fss::transport::asset_command_altitude: return "ALT";
        case fss::transport::asset_command_manual: return "MAN";
        default: return "UNKNOWN";
    }
}

class logging_client : public fss::client_ssl::fss_client {
public:
    using fss::client_ssl::fss_client::fss_client;
    void handleCommandFrom(const std::shared_ptr<fss::transport::fss_message_asset_command> &msg,
                           fss::client_ssl::fss_server *origin) override
    {
        std::cout << "RCVD_CMD: " << commandName(msg->getCommand()) << std::endl;
        /* When the server negotiated command-ack, reply on the originating
         * connection with the two-phase ack (received, then actioned) so the
         * server's routing+storage path is exercised end to end. A real FMU
         * picks the terminal outcome from its state machine; this example always
         * reports "actioned" since it has no flight logic. */
        auto conn = origin->getConnection();
        if (conn == nullptr || (conn->getNegotiatedFeatureFlags() & fss::transport::FSS_FEATURE_COMMAND_ACK) == 0)
        {
            return;
        }
        const uint64_t acked_id = msg->getId();
        const auto command = msg->getCommand();
        origin->sendMsg(std::make_shared<fss::transport::fss_message_command_ack>(
            acked_id, command, fss::transport::command_ack_received, fss::fss_current_timestamp()));
        origin->sendMsg(std::make_shared<fss::transport::fss_message_command_ack>(
            acked_id, command, fss::transport::command_ack_actioned, fss::fss_current_timestamp()));
        std::cout << "SENT_ACK: " << commandName(command) << std::endl;
    }
};

auto main(int argc, char *argv[]) -> int
{
    if (argc < 2)
    {
        std::cout << "Insufficient arguments" << std::endl;
        return -1;
    }
    signal(SIGINT, sigIntHandler);
    /* Writing to a peer that has reset the connection (e.g. after a CRL
     * revocation disconnect) raises SIGPIPE; the default action kills the
     * process instead of letting the blocking send just fail, same reason
     * server.cpp ignores it. */
    signal(SIGPIPE, SIG_IGN);

    auto client = std::make_shared<logging_client>(argv[1]);

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
    while (running == 1)
    {
        sleep(1);
        client->attemptReconnect();
        counter++;
        if (counter % send_interval == 0)
        {
            constexpr int bat_remaining = 75;
            constexpr int bat_mah_used = 1000;
            constexpr double bat_voltage = 11.4;
            auto msg_status =
                std::make_shared<fss::transport::fss_message_system_status>(bat_remaining, bat_mah_used, bat_voltage);
            constexpr int search_number = 1;
            constexpr int search_current_point = 23;
            constexpr int search_total_points = 100;
            auto msg_search = std::make_shared<fss::transport::fss_message_search_status>(
                search_number, search_current_point, search_total_points);
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
            auto msg_pos = std::make_shared<fss::transport::fss_message_position_report>(
                lat, lng, alt, heading_cdeg, hor_vel, vert_vel, icao_address, callsign, squawk_code,
                time_since_last_contact, flags, alt_type, emitter_type, fss::fss_current_timestamp());
            client->sendMsgAll(msg_status);
            client->sendMsgAll(msg_search);
            client->sendMsgAll(msg_pos);
        }
    }
    client->disconnect();
}
