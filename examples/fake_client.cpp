#include "fss-client-ssl.hpp"

#include <cstdlib>
#include <csignal>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

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
    /* Logged in the same grep-able style as RCVD_CMD/SENT_ACK below, so e2e
     * tests can assert a client actually received a relayed position report
     * (todo/28) by scanning its stdout log rather than the DB. */
    void handlePositionReport(const std::shared_ptr<fss::transport::fss_message_position_report> &msg) override
    {
        std::ostringstream icao_hex;
        icao_hex << std::hex << msg->getICAOAddress();
        std::cout << "RCVD_POS: icao=" << icao_hex.str() << " callsign=" << msg->getCallSign()
                  << " lat=" << msg->getLatitude() << " lng=" << msg->getLongitude() << std::endl;
    }
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

namespace {

/* CLI-overridable knobs for driving an ADS-B-style feeder (todo/28). Every
 * default reproduces this example's original fixed behaviour exactly, so
 * existing callers that only pass a config path are unaffected. */
struct cli_options {
    uint32_t icao_address{0};
    std::string callsign{"example"};
    int position_interval_ms{5000};
};

auto parse_args(int argc, char *argv[]) -> cli_options
{
    cli_options opts;
    constexpr std::string_view icao_prefix = "--icao=";
    constexpr std::string_view callsign_prefix = "--callsign=";
    constexpr std::string_view interval_prefix = "--position-interval-ms=";
    for (int i = 2; i < argc; i++)
    {
        std::string_view arg = argv[i];
        if (arg.substr(0, icao_prefix.size()) == icao_prefix)
        {
            opts.icao_address =
                static_cast<uint32_t>(std::strtoul(std::string(arg.substr(icao_prefix.size())).c_str(), nullptr, 0));
        }
        else if (arg.substr(0, callsign_prefix.size()) == callsign_prefix)
        {
            opts.callsign = std::string(arg.substr(callsign_prefix.size()));
        }
        else if (arg.substr(0, interval_prefix.size()) == interval_prefix)
        {
            opts.position_interval_ms = std::atoi(std::string(arg.substr(interval_prefix.size())).c_str());
        }
        else
        {
            std::cout << "Unknown argument: " << arg << std::endl;
        }
    }
    return opts;
}

} // namespace

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
    auto opts = parse_args(argc, argv);

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

    /* Base tick fine-grained enough to drive a fast position-interval for
     * rate-limit testing (todo/28 wants well over 20/s to exceed the
     * default token-bucket capacity within a short test), while
     * status/search keep their original fixed 5s cadence regardless of the
     * position interval. */
    constexpr int tick_ms = 10;
    constexpr int reconnect_check_ms = 1000;
    constexpr int status_search_interval_ms = 5000;
    int elapsed_ms = 0;
    while (running == 1)
    {
        usleep(tick_ms * 1000);
        elapsed_ms += tick_ms;
        if ((elapsed_ms % reconnect_check_ms) == 0)
        {
            client->attemptReconnect();
        }
        if ((elapsed_ms % status_search_interval_ms) == 0)
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
            client->sendMsgAll(msg_status);
            client->sendMsgAll(msg_search);
        }
        if (opts.position_interval_ms > 0 && (elapsed_ms % opts.position_interval_ms) == 0)
        {
            constexpr double lat = -43.5;
            constexpr double lng = 172.5;
            constexpr int alt = 300;
            constexpr int heading_cdeg = 1800;
            constexpr int hor_vel = 200;
            constexpr int vert_vel = 0;
            constexpr int squawk_code = 01200;
            constexpr int time_since_last_contact = 0;
            constexpr int flags = 1 | 2 | 4 | 8 | 16 | 32;
            constexpr int alt_type = 1;
            constexpr int emitter_type = 14;
            auto msg_pos = std::make_shared<fss::transport::fss_message_position_report>(
                lat, lng, alt, heading_cdeg, hor_vel, vert_vel, opts.icao_address, opts.callsign, squawk_code,
                time_since_last_contact, flags, alt_type, emitter_type, fss::fss_current_timestamp());
            client->sendMsgAll(msg_pos);
        }
    }
    client->disconnect();
}
