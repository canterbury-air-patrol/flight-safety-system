#include "fss-client-ssl.hpp"

#include <cstdlib>
#include <csignal>
#include <iostream>
#include <optional>
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
    /* Exposes the protected setter for --clock-offset-ms (todo/33): a CLI
     * flag is "less magic" than faketime and works on any runner. */
    void applyClockOffsetMs(int64_t offset_ms) { this->setClockOffsetMs(offset_ms); }
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
    /* The server sends RTT requests from its per-second liveness tick, so
     * this line is an externally observable "the main loop still runs"
     * signal — e2e's DB black-hole test counts these while the DB is wedged
     * (todo/46). */
    void handleRTTRequest(const std::shared_ptr<fss::transport::fss_message_rtt_request> &msg) override
    {
        std::cout << "RCVD_RTT_REQ: id=" << msg->getId() << std::endl;
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
    int64_t clock_offset_ms{0};
};

/* Parses `text` as a base-10 integer via strtoul/strtol, requiring the
 * *entire* string to be consumed (no trailing junk, no empty string) so a
 * malformed --flag=value fails loudly instead of silently becoming 0 --
 * a bad e2e test invocation should error out, not run with nonsense
 * defaults. */
auto parse_uint_arg(std::string_view flag, std::string_view text, uint32_t &out) -> bool
{
    if (text.empty())
    {
        std::cout << "Invalid value for " << flag << ": empty" << std::endl;
        return false;
    }
    std::string text_owned(text);
    char *end = nullptr;
    unsigned long value = std::strtoul(text_owned.c_str(), &end, 0);
    if (end != text_owned.c_str() + text_owned.size())
    {
        std::cout << "Invalid value for " << flag << ": '" << text << "'" << std::endl;
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

auto parse_int_arg(std::string_view flag, std::string_view text, int &out) -> bool
{
    if (text.empty())
    {
        std::cout << "Invalid value for " << flag << ": empty" << std::endl;
        return false;
    }
    std::string text_owned(text);
    char *end = nullptr;
    long value = std::strtol(text_owned.c_str(), &end, 10);
    if (end != text_owned.c_str() + text_owned.size())
    {
        std::cout << "Invalid value for " << flag << ": '" << text << "'" << std::endl;
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

auto parse_args(int argc, char *argv[]) -> std::optional<cli_options>
{
    cli_options opts;
    constexpr std::string_view icao_prefix = "--icao=";
    constexpr std::string_view callsign_prefix = "--callsign=";
    constexpr std::string_view interval_prefix = "--position-interval-ms=";
    constexpr std::string_view clock_offset_prefix = "--clock-offset-ms=";
    for (int i = 2; i < argc; i++)
    {
        std::string_view arg = argv[i];
        if (arg.substr(0, icao_prefix.size()) == icao_prefix)
        {
            if (!parse_uint_arg(icao_prefix, arg.substr(icao_prefix.size()), opts.icao_address))
            {
                return std::nullopt;
            }
        }
        else if (arg.substr(0, callsign_prefix.size()) == callsign_prefix)
        {
            opts.callsign = std::string(arg.substr(callsign_prefix.size()));
        }
        else if (arg.substr(0, interval_prefix.size()) == interval_prefix)
        {
            if (!parse_int_arg(interval_prefix, arg.substr(interval_prefix.size()), opts.position_interval_ms))
            {
                return std::nullopt;
            }
        }
        else if (arg.substr(0, clock_offset_prefix.size()) == clock_offset_prefix)
        {
            opts.clock_offset_ms =
                std::strtoll(std::string(arg.substr(clock_offset_prefix.size())).c_str(), nullptr, 10);
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
    auto parsed_opts = parse_args(argc, argv);
    if (!parsed_opts.has_value())
    {
        return -1;
    }
    auto opts = *parsed_opts;
    if (opts.clock_offset_ms != 0)
    {
        client->applyClockOffsetMs(opts.clock_offset_ms);
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

    /* Base tick fine-grained enough to drive a fast position-interval for
     * rate-limit testing (todo/28 wants well over 20/s to exceed the
     * default token-bucket capacity within a short test), while
     * status/search keep their original fixed 5s cadence regardless of the
     * position interval.
     *
     * Deadline-based (next_*_ms), not modulo-on-elapsed_ms: elapsed_ms only
     * ever takes multiples of tick_ms, so `elapsed_ms % interval_ms == 0`
     * only fires on schedule when interval_ms happens to be a multiple of
     * tick_ms -- any other value (e.g. --position-interval-ms=1234) fires
     * at LCM(tick_ms, interval_ms) instead, silently far slower than
     * requested. Each next_*_ms is incremented by its own interval after
     * firing, so drift never accumulates and any positive interval works. */
    constexpr int tick_ms = 10;
    constexpr int reconnect_check_ms = 1000;
    constexpr int status_search_interval_ms = 5000;
    int elapsed_ms = 0;
    int next_reconnect_ms = reconnect_check_ms;
    int next_status_search_ms = status_search_interval_ms;
    int next_position_ms = opts.position_interval_ms;
    while (running == 1)
    {
        usleep(tick_ms * 1000);
        elapsed_ms += tick_ms;
        if (elapsed_ms >= next_reconnect_ms)
        {
            client->attemptReconnect();
            next_reconnect_ms += reconnect_check_ms;
        }
        if (elapsed_ms >= next_status_search_ms)
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
            next_status_search_ms += status_search_interval_ms;
        }
        if (opts.position_interval_ms > 0 && elapsed_ms >= next_position_ms)
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
            /* getSkewedTimestamp() (todo/33) rather than fss_current_timestamp()
             * directly: a genuinely clock-skewed client stamps everything with
             * its wrong clock, not just RTT responses -- defaults to the real
             * clock when --clock-offset-ms was never given. */
            auto msg_pos = std::make_shared<fss::transport::fss_message_position_report>(
                lat, lng, alt, heading_cdeg, hor_vel, vert_vel, opts.icao_address, opts.callsign, squawk_code,
                time_since_last_contact, flags, alt_type, emitter_type, client->getSkewedTimestamp());
            client->sendMsgAll(msg_pos);
            next_position_ms += opts.position_interval_ms;
        }
    }
    client->disconnect();
}
