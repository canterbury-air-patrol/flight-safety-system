#include <fss-transport-ssl.hpp>
#include <fss.hpp>

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <random>

namespace flight_safety_system {
namespace client_ssl {

class fss_client;
class fss_server;

enum connection_status {
    CLIENT_CONNECTION_STATUS_UNKNOWN,
    CLIENT_CONNECTION_STATUS_CONNECTED_1_SERVER,
    CLIENT_CONNECTION_STATUS_CONNECTED_2_OR_MORE,
    CLIENT_CONNECTION_STATUS_DISCONNECTED,
};

class fss_client {
private:
    std::string asset_name{""};
    /* When true, sendIdentify() sends message_type_identity_non_aircraft
     * instead of the aircraft identity (todo/28). The server derives the
     * asset name from the peer cert's CN in that path, not from a message
     * field, so there is no non-aircraft counterpart to asset_name. */
    bool non_aircraft{false};
    /* Added to the wall-clock timestamp reported in an RTT response, when
     * the server negotiated the clock-offset capability (todo/33). Lets an
     * e2e test drive a client with a deliberately skewed clock without
     * needing root/faketime. Positive = client clock ahead of real time. */
    int64_t clock_offset_ms{0};
    std::string ca_file{""};
    std::string private_key_file{""};
    std::string public_key_file{""};
    std::list<std::shared_ptr<flight_safety_system::client_ssl::fss_server>> servers{};
    std::list<std::shared_ptr<flight_safety_system::client_ssl::fss_server>> reconnect_servers{};
    /* servers_lock guards the server lists and the derived `configured` flag.
     * It does NOT guard asset_name, which is configuration state set before the
     * client is used concurrently (mutable so the const isConfigured() can lock
     * it to read the flag). */
    mutable std::mutex servers_lock{};
    bool configured{false}; // guarded by servers_lock
    /* Recompute `configured` from the current asset name + server lists. Called
     * from every path that sets the name or adds a server so isConfigured()
     * stays accurate however the client was built, not just the file ctor.
     * Takes servers_lock itself, so callers must not already hold it. */
    void updateConfigured();
    void notifyConnectionStatus();
    virtual void connectionStatusChange(flight_safety_system::client_ssl::connection_status status);
protected:
    void setAssetName(std::string t_asset_name);
    void setNonAircraft(bool t_non_aircraft);
    void setClockOffsetMs(int64_t t_offset_ms);
    void addServer(const std::shared_ptr<fss_server> &server);
public:
    explicit fss_client(const std::string &config_file);
    explicit fss_client();
    explicit fss_client(std::string t_ca, std::string t_private_key, std::string t_public_key);
    fss_client(fss_client &) = delete;
    fss_client(fss_client &&) = delete;
    auto operator=(fss_client &) -> fss_client & = delete;
    auto operator=(fss_client &&) -> fss_client & = delete;
    virtual ~fss_client();
    virtual void connectTo(const std::string &t_address, uint16_t t_port, bool connect);
    virtual void attemptReconnect();
    virtual void disconnect();
    virtual void sendMsgAll(const std::shared_ptr<flight_safety_system::transport::fss_message> &msg);
    virtual auto getAssetName() -> std::string;
    virtual auto isNonAircraft() const -> bool { return this->non_aircraft; }
    virtual auto getClockOffsetMs() const -> int64_t { return this->clock_offset_ms; }
    /* fss_current_timestamp() + clock_offset_ms (todo/33): the single
     * source of truth for "what time does this client think it is", used
     * both for the RTT response's reported client clock and for any
     * message timestamp a skewed-clock test wants to be consistent with
     * it (e.g. a position report) -- a client whose clock is genuinely
     * wrong stamps everything with that wrong clock, not just RTT
     * responses. */
    virtual auto getSkewedTimestamp() const -> uint64_t;
    virtual auto isConfigured() const -> bool;
    virtual void serverRequiresReconnect(fss_server *server);
    virtual void updateServers(const std::shared_ptr<flight_safety_system::transport::fss_message_server_list> &msg);
    /* Called for every command, with the originating server (the connection the
     * command arrived on). A subclass that needs to reply to that specific
     * connection — e.g. to send a command-ack, whose id and negotiated feature
     * flags are per-connection — overrides this and uses
     * origin->sendMsg()/origin->getConnection(). The default delegates to the
     * connection-agnostic handleCommand() below, so a subclass that only cares
     * about the command (the common case) overrides that one and is unaffected.
     * Distinct name rather than an overload so neither hides the other. */
    virtual void
    handleCommandFrom(const std::shared_ptr<flight_safety_system::transport::fss_message_asset_command> &msg,
                      flight_safety_system::client_ssl::fss_server *origin);
    virtual void handleCommand(const std::shared_ptr<flight_safety_system::transport::fss_message_asset_command> &msg
                               __attribute__((unused)));
    virtual void
    handlePositionReport(const std::shared_ptr<flight_safety_system::transport::fss_message_position_report> &msg
                         __attribute__((unused)));
    virtual void handleSMMSettings(const std::shared_ptr<flight_safety_system::transport::fss_message_smm_settings> &msg
                                   __attribute__((unused)));
};

class fss_server : public flight_safety_system::transport::fss_message_cb {
private:
    fss_client *client;
    std::string address;
    uint16_t port;
    std::string ca_file{""};
    std::string private_key_file{""};
    std::string public_key_file{""};
    /* Backoff bookkeeping (last_tried, retry_count, retry_delay,
     * effective_delay, rng) is touched only from the application thread that
     * drives reconnection — the caller of attemptReconnect() / reconnect()
     * and connectTo(..., true). The recv thread must NOT write these
     * directly: when a connection closes it requests a reset via the atomic
     * backoff_reset_requested flag, which reconnect() consumes. */
    uint64_t last_tried{0};
    uint64_t retry_count{0};
    static constexpr uint64_t retry_delay_start = 1000;
    static constexpr uint64_t retry_delay_cap = 30000;
    uint64_t retry_delay{retry_delay_start};
    uint64_t effective_delay{retry_delay_start};
    std::mt19937 rng{std::random_device{}()};
    std::atomic<bool> backoff_reset_requested{false};
    void resetBackoff();
    std::shared_ptr<flight_safety_system::IClock> clock{std::make_shared<flight_safety_system::MonotonicClock>()};
    std::atomic<bool> liveness_active{false};
    std::atomic<uint64_t> last_message_received_time{0};
    uint64_t server_timeout_ms{30000};
protected:
    virtual auto reconnect_to() -> bool;
public:
    fss_server(fss_client *t_client, std::string t_address, uint16_t t_port, std::string t_ca,
               std::string t_private_key, std::string t_public_key);
    fss_server(fss_server &other) = delete;
    fss_server(fss_server &&) = delete;
    auto operator=(fss_server &) -> fss_server & = delete;
    auto operator=(fss_server &&) -> fss_server & = delete;
    ~fss_server() override;
    void processMessage(std::shared_ptr<flight_safety_system::transport::fss_message> message) override;
    virtual auto getAddress() -> std::string;
    virtual auto getPort() -> uint16_t;
    virtual auto reconnect() -> bool;
    virtual auto getClient() -> fss_client *;
    virtual void sendIdentify();
    virtual void sendVersion();
    void setClock(std::shared_ptr<flight_safety_system::IClock> t_clock);
    void setServerTimeoutMs(uint64_t ms) { this->server_timeout_ms = ms; }
    auto isServerTimedOut() -> bool;
    auto getEffectiveDelay() const -> uint64_t { return this->effective_delay; }
};


} // namespace client_ssl
} // namespace flight_safety_system