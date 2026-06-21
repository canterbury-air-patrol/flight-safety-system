#include "fss-transport.hpp"
#include "fss-transport-ssl.hpp"
#include "fss-log.hpp"
#include "fss.hpp"
#include "fss-server.hpp"
#include "server-clients.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <fstream>
#include <csignal>
#include <list>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
#include <json/json.h>
#pragma GCC diagnostic pop

#include <unistd.h>

namespace flight_safety_system::server {
/* Defined in client_session.cpp. Exposed here so the periodic broadcast
 * of the active server list can reuse the helper. */
auto build_server_list_msg(IDatabase *dbc) -> std::shared_ptr<transport::fss_message_server_list>;
} // namespace flight_safety_system::server


volatile sig_atomic_t running = 1;
volatile sig_atomic_t reload_crl = 0;

void sigIntHandler(int signum __attribute__((unused)))
{
    running = 0;
}

void sigHupHandler(int signum __attribute__((unused)))
{
    reload_crl = 1;
}

auto main(int argc, char *argv[]) -> int
{
    struct sigaction sa_int = {};
    sa_int.sa_handler = sigIntHandler;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = 0;
    sigaction(SIGINT, &sa_int, nullptr);
    struct sigaction sa_term = {};
    sa_term.sa_handler = sigIntHandler;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    sigaction(SIGTERM, &sa_term, nullptr);
    struct sigaction sa_hup = {};
    sa_hup.sa_handler = sigHupHandler;
    sigemptyset(&sa_hup.sa_mask);
    sa_hup.sa_flags = 0;
    sigaction(SIGHUP, &sa_hup, nullptr);
    struct sigaction sa_pipe = {};
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sa_pipe.sa_flags = 0;
    sigaction(SIGPIPE, &sa_pipe, nullptr);
    std::string conf_file = (argc > 1 ? std::string(argv[1]) : "/etc/fss/server.json");
    std::ifstream configfile(conf_file);
    if (!configfile.is_open())
    {
        FSS_LOG_ERROR("server", "Failed to load configuration: " << conf_file);
        return 1;
    }
    /* Read the whole configuration inside one try block: jsoncpp throws
     * Json::Exception on malformed JSON and on wrong-typed values (e.g.
     * "port": "8080"), and an uncaught throw here would std::terminate
     * without saying which field is bad. All values land in locals so no
     * config access happens after this block. */
    constexpr int default_pg_port = 5432;
    constexpr std::size_t default_db_queue_depth = 10000;
    constexpr int default_client_timeout_sec = 30;
    constexpr int default_identify_timeout_sec = 30;
    constexpr uint64_t default_position_staleness_ms = flight_safety_system::server::default_position_staleness_ms;
    constexpr uint64_t default_rate_capacity = 100;
    constexpr uint64_t default_rate_refill_per_s = 20;
    constexpr unsigned int default_tls_handshake_timeout_ms =
        flight_safety_system::transport_ssl::default_handshake_timeout_ms;
    constexpr std::size_t default_max_concurrent_handshakes = 64;
    int listen_port = 0;
    int pg_port = default_pg_port;
    std::string pg_host;
    std::string pg_user;
    std::string pg_pass;
    std::string pg_db;
    std::size_t db_queue_depth = default_db_queue_depth;
    uint64_t client_timeout_sec = default_client_timeout_sec;
    uint64_t identify_timeout_sec = default_identify_timeout_sec;
    uint64_t position_staleness_ms = default_position_staleness_ms;
    uint64_t rate_capacity = default_rate_capacity;
    uint64_t rate_refill = default_rate_refill_per_s;
    unsigned int tls_handshake_timeout_ms = default_tls_handshake_timeout_ms;
    std::size_t max_concurrent_handshakes = default_max_concurrent_handshakes;
    std::string ca_public_key;
    std::string server_private_key;
    std::string server_public_key;
    std::string crl_file;
    try
    {
        Json::Value config;
        configfile >> config;

        if (config.isMember("log_level"))
        {
            flight_safety_system::log::set_level(config["log_level"].asString());
        }

        auto cfg_require = [&](const char *path, const Json::Value &node, const char *key) -> bool {
            if (!node.isMember(key) || node[key].asString().empty())
            {
                FSS_LOG_ERROR("server", "Missing required config field: " << path << "." << key);
                return false;
            }
            return true;
        };
        bool cfg_ok = true;
        if (!config.isMember("port"))
        {
            FSS_LOG_ERROR("server", "Missing required config field: port");
            cfg_ok = false;
        }
        cfg_ok &= cfg_require("postgres", config["postgres"], "host");
        cfg_ok &= cfg_require("postgres", config["postgres"], "user");
        cfg_ok &= cfg_require("postgres", config["postgres"], "pass");
        cfg_ok &= cfg_require("postgres", config["postgres"], "db");
        cfg_ok &= cfg_require("ssl", config["ssl"], "ca_public_key");
        cfg_ok &= cfg_require("ssl", config["ssl"], "server_private_key");
        cfg_ok &= cfg_require("ssl", config["ssl"], "server_public_key");
        if (!cfg_ok)
        {
            return 1;
        }

        listen_port = config["port"].asInt();
        if (config["postgres"].isMember("port"))
        {
            pg_port = config["postgres"]["port"].asInt();
        }
        pg_host = config["postgres"]["host"].asString();
        pg_user = config["postgres"]["user"].asString();
        pg_pass = config["postgres"]["pass"].asString();
        pg_db = config["postgres"]["db"].asString();
        if (config.isMember("db_queue_depth"))
        {
            db_queue_depth = config["db_queue_depth"].asUInt();
        }
        if (config.isMember("client_timeout"))
        {
            client_timeout_sec = config["client_timeout"].asUInt64();
        }
        if (config.isMember("identify_timeout"))
        {
            identify_timeout_sec = config["identify_timeout"].asUInt64();
        }
        if (config.isMember("position_staleness_ms"))
        {
            position_staleness_ms = config["position_staleness_ms"].asUInt64();
        }
        if (config.isMember("message_rate_capacity"))
        {
            rate_capacity = config["message_rate_capacity"].asUInt64();
        }
        if (config.isMember("message_rate_refill"))
        {
            rate_refill = config["message_rate_refill"].asUInt64();
        }
        if (config.isMember("tls_handshake_timeout_ms"))
        {
            tls_handshake_timeout_ms = config["tls_handshake_timeout_ms"].asUInt();
        }
        if (tls_handshake_timeout_ms == 0)
        {
            /* gnutls treats a 0 ms handshake timeout as "never time out", which
             * re-opens the stalled-handshake DoS this setting exists to close. */
            FSS_LOG_WARN("server",
                         "tls_handshake_timeout_ms must be > 0 (0 disables the handshake timeout); using default "
                             << default_tls_handshake_timeout_ms);
            tls_handshake_timeout_ms = default_tls_handshake_timeout_ms;
        }
        if (config.isMember("max_concurrent_handshakes"))
        {
            max_concurrent_handshakes = config["max_concurrent_handshakes"].asUInt();
        }
        if (max_concurrent_handshakes == 0)
        {
            /* A bound of 0 would refuse every incoming connection. */
            FSS_LOG_WARN("server",
                         "max_concurrent_handshakes must be > 0; using default " << default_max_concurrent_handshakes);
            max_concurrent_handshakes = default_max_concurrent_handshakes;
        }
        ca_public_key = config["ssl"]["ca_public_key"].asString();
        server_private_key = config["ssl"]["server_private_key"].asString();
        server_public_key = config["ssl"]["server_public_key"].asString();
        if (config["ssl"].isMember("crl_file"))
        {
            crl_file = config["ssl"]["crl_file"].asString();
        }
    }
    catch (const Json::Exception &e)
    {
        FSS_LOG_ERROR("server", "Invalid configuration in " << conf_file << ": " << e.what());
        return 1;
    }

    auto dbc = std::make_shared<flight_safety_system::server::db_connection>(pg_host, pg_port, pg_user, pg_pass, pg_db);

    if (!dbc->isConnected())
    {
        FSS_LOG_ERROR("server", "Failed to connect to database; aborting");
        return 1;
    }

    flight_safety_system::server::db_write_sink sink =
        [dbc](const flight_safety_system::server::db_write_task &task) -> void {
        std::visit(
            flight_safety_system::server::overloaded{
                [&](const flight_safety_system::server::rtt_write &w) -> void { dbc->recordRtt(w.asset_id, w.rtt_ms); },
                [&](const flight_safety_system::server::position_write &w) -> void {
                    dbc->recordPosition(w.asset_id, w.latitude, w.longitude, w.altitude);
                },
                [&](const flight_safety_system::server::status_write &w) -> void {
                    dbc->recordStatus(w.asset_id, w.bat_percent, w.bat_mah_used, w.bat_voltage);
                },
                [&](const flight_safety_system::server::search_status_write &w) -> void {
                    dbc->recordSearchStatus(w.asset_id, w.search_id, w.completed, w.total);
                },
                [&](const flight_safety_system::server::command_dispatch_write &w) -> void {
                    dbc->recordCommandDispatch(w.command_dbid, w.dispatch_id);
                },
                [&](const flight_safety_system::server::command_ack_write &w) -> void {
                    dbc->recordCommandAck(w.asset_id, w.dispatch_id, w.ack_state, w.ack_timestamp, w.ack_reason);
                },
            },
            task);
    };
    auto writer = std::make_shared<flight_safety_system::server::db_write_queue>(db_queue_depth, sink);

    auto clients = std::make_shared<server_clients>();
    constexpr int msec_per_sec = 1000;
    clients->setClientTimeoutMs(client_timeout_sec * msec_per_sec);
    clients->setClientIdentifyTimeoutMs(identify_timeout_sec * msec_per_sec);
    clients->setClientStalenessMs(position_staleness_ms);
    clients->setClientRateLimits(rate_capacity, rate_refill);

    std::shared_ptr<flight_safety_system::transport::fss_listen> listen;
    FSS_LOG_INFO("server", "Starting fss server in TLS mode");
    if (!crl_file.empty())
    {
        FSS_LOG_INFO("server", "CRL file configured: " << crl_file);
    }
    flight_safety_system::transport::fss_connect_cb connect_cb =
        [dbc, writer, &clients](std::shared_ptr<flight_safety_system::transport::fss_connection> conn) -> bool {
#ifdef DEBUG
        std::cout << "New client connected" << std::endl;
#endif
        clients->clientConnected(std::make_shared<flight_safety_system::server::fss_client>(std::move(conn), dbc.get(),
                                                                                            writer, clients.get()));
        return true;
    };
    listen = std::make_shared<flight_safety_system::transport_ssl::fss_listen>(
        listen_port, connect_cb, ca_public_key, server_private_key, server_public_key, crl_file,
        tls_handshake_timeout_ms, max_concurrent_handshakes);

    /* Main-loop DB contract: the loop below must make NO synchronous DB
     * reads. Reads are handled exclusively by the command_poller thread
     * below, which caches results: pending commands and SMM settings on
     * each fss_client, and the active-server-list message on
     * server_clients. The loop only calls sendCommand()/sendSMMSettings()/
     * broadcastMsg() (all read caches), write-queue enqueues (async), and
     * in-memory bookkeeping. A DB stall therefore cannot block heartbeats,
     * timeout monitoring, or command dispatch.
     *
     * Split tick: sendCommand runs every command_poll_ms so safety-critical
     * commands (TERM, DISARM) reach aircraft in <=100ms instead of <=1s.
     * Per-second tasks (RTT, timeout sweep) and per-15s tasks (server list,
     * SMM settings) retain their original cadence via the tick counter. */
    constexpr int command_poll_ms = flight_safety_system::server::command_poll_ms;
    static_assert(1000 % command_poll_ms == 0, "command_poll_ms must evenly divide 1000 to avoid tick skew");
    static_assert(1000 / command_poll_ms > 0, "command_poll_ms must be <= 1000ms");

    constexpr int usec_per_msec = 1000;
    constexpr int ticks_per_sec = 1000 / command_poll_ms;
    constexpr int send_config_period_ticks = 15 * ticks_per_sec;
    std::atomic<bool> poll_running{true};
    std::thread command_poller([&clients, &dbc, &poll_running, command_poll_ms]() -> void {
        flight_safety_system::exception_guard poll_guard("server", "pollCommands");
        uint64_t poll_counter = 0;
        while (poll_running.load())
        {
            poll_guard.run([&]() -> void {
                clients->pollCommands(dbc.get());
                /* Config reads share the poller so the main loop never
                 * touches the DB. First refresh happens immediately
                 * (counter 0) so the caches are primed at startup. */
                if ((poll_counter % send_config_period_ticks) == 0)
                {
                    clients->setCachedServerList(flight_safety_system::server::build_server_list_msg(dbc.get()));
                    clients->refreshSmmSettings();
                }
            });
            poll_counter++;
            std::this_thread::sleep_for(std::chrono::milliseconds(command_poll_ms));
        }
    });

    uint64_t tick_counter = 0;
    flight_safety_system::exception_guard tick_guard("server", "main loop tick");
    while (running == 1)
    {
        if (reload_crl == 1)
        {
            reload_crl = 0;
            FSS_LOG_INFO("server", "SIGHUP received — reloading CRL");
            /* The listener is not rebuilt: each new connection loads the CRL
             * file fresh during its TLS handshake (see setupSession), so new
             * connections already honour an updated CRL. Tearing down and
             * rebinding the listener here only risks a window with no listener
             * (or, on a failed rebind, a dead listener). We only need to drop
             * already-established sessions whose certs are now revoked. */
            if (!crl_file.empty())
            {
                auto disconnected = clients->disconnectRevokedClients(crl_file);
                FSS_LOG_INFO("server", "CRL reload complete: disconnected " << disconnected << " revoked session(s)");
            }
            else
            {
                FSS_LOG_INFO("server", "CRL reload: no CRL file configured; nothing to do");
            }
        }
        usleep(command_poll_ms * usec_per_msec);
        tick_guard.run([&]() -> void {
            clients->sendCommand();
            if ((tick_counter % ticks_per_sec) == 0)
            {
                clients->cleanupRemovableClients();
                clients->checkTimeouts();
                clients->sendRTTRequest();
                static uint64_t last_failure_count = 0;
                uint64_t current_failures = writer->write_failure_count();
                if (current_failures != last_failure_count)
                {
                    FSS_LOG_ERROR("server", "DB write failures since start: " << current_failures);
                    last_failure_count = current_failures;
                }
                static uint64_t last_command_dropped = 0;
                uint64_t current_command_dropped = writer->command_dropped_count();
                if (current_command_dropped != last_command_dropped)
                {
                    FSS_LOG_ERROR("server", "command DB writes dropped since start: " << current_command_dropped);
                    last_command_dropped = current_command_dropped;
                }
                dbc->tryReconnectIfNeeded();
            }
            if ((tick_counter % send_config_period_ticks) == 0)
            {
                /* Cache-only: the poller builds the server list and refreshes
                 * the per-client SMM settings. Empty cache (startup race with
                 * the poller's first pass) just skips one broadcast; clients
                 * also receive the list directly at identify time. */
                auto server_list = clients->getCachedServerList();
                if (server_list != nullptr)
                {
                    clients->broadcastMsg(server_list);
                }
                clients->sendSMMSettings();
            }
        });
        tick_counter++;
    }

    /* Explicit shutdown ordering: stop accepting, then stop the command
     * poller (which holds dbc), then tear down clients, then the write
     * queue, then the DB connection. */
    listen.reset();
    poll_running.store(false);
    command_poller.join();
    clients.reset();
    writer->stop();
    writer.reset();
    dbc.reset();
}
