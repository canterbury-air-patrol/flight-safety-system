#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <sys/types.h>
#include <thread>
#include <list>

#include "fss.hpp"
#include "secure-string.hpp"

namespace flight_safety_system {

namespace transport {

static constexpr double FSS_COORD_SCALE = 0.0000001;

/* Fixed-point scale for battery voltage on the wire (volts per LSB). Held
 * separate from FSS_COORD_SCALE — they share a value today but are unrelated
 * quantities; changing coordinate precision must not silently rescale
 * voltages. */
static constexpr double FSS_VOLTAGE_SCALE = 0.0000001;

/* Wire-protocol version handshake.
 * - FSS_PROTOCOL_VERSION_LEGACY (0): unversioned protocol that pre-dates the
 *   handshake. Used as the negotiated value when the peer never sends a
 *   version message (e.g. an old client still in the field).
 * - FSS_PROTOCOL_VERSION: the highest version this build speaks.
 * - FSS_PROTOCOL_MIN_VERSION: the lowest version this build still accepts.
 *   If the peer's max < our min (or vice-versa), we disconnect. */
static constexpr uint16_t FSS_PROTOCOL_VERSION_LEGACY = 0;
static constexpr uint16_t FSS_PROTOCOL_VERSION = 2;
static constexpr uint16_t FSS_PROTOCOL_MIN_VERSION = 1;

/* Optional-capability negotiation. The version handshake's feature_flags is a
 * bitmask of capabilities each peer supports; the negotiated set is the
 * intersection (peer's flags & FSS_SUPPORTED_FEATURES), so a feature is only
 * used when BOTH peers advertise it. This lets old peers interop unchanged
 * (they advertise 0) without a protocol-version bump — see todo/17. A peer
 * cannot force on a capability we do not implement: the AND with
 * FSS_SUPPORTED_FEATURES masks any bit we have not enabled. Each bit is added
 * to FSS_SUPPORTED_FEATURES only once this build actually implements it. */
static constexpr uint32_t FSS_FEATURE_RTT_OFFSET = 0x1U;    /* todo/17 item 3 */
static constexpr uint32_t FSS_FEATURE_COMMAND_ACK = 0x2U;   /* todo/17 item 1 */
static constexpr uint32_t FSS_FEATURE_SYSTEM_HEALTH = 0x4U; /* todo/17 item 2 */
static constexpr uint32_t FSS_SUPPORTED_FEATURES = FSS_FEATURE_RTT_OFFSET;

/* The capability set to adopt for a peer that advertised peer_flags: the
 * intersection with what this build implements, so a peer can never enable a
 * feature we do not support. Both handshake paths (server: client_session.cpp,
 * client: client-ssl.cpp) negotiate through this one function so the policy
 * cannot drift between them. */
inline auto negotiateFeatureFlags(uint32_t peer_flags) -> uint32_t
{
    return peer_flags & FSS_SUPPORTED_FEATURES;
}

/* Maximum payload length accepted from the wire. Anything larger is rejected
 * before allocation to prevent memory exhaustion attacks. Sized well above the
 * largest legitimate message (server_list with many entries) with headroom. */
static constexpr uint16_t FSS_MAX_MESSAGE_BYTES = 8192;

class fss_connection;
class fss_listen;
class fss_message;

using fss_connect_cb = std::function<bool(std::shared_ptr<fss_connection>)>;

using fss_message_type = enum fss_message_type_e {
    message_type_unknown,
    /* Socket closed */
    message_type_closed,
    /* On connect */
    message_type_identity,

    /* Either way */
    message_type_rtt_request,
    message_type_rtt_response,

    /* From the client */
    message_type_position_report,
    message_type_system_status,
    message_type_search_status,

    /* From the server */
    message_type_command,
    message_type_server_list,
    message_type_smm_settings,

    /* Non-aircraft clients */
    message_type_identity_non_aircraft,

    /* Please send identity */
    message_type_identity_required,

    /* Protocol version handshake. Sent first by both peers after the TLS
     * handshake; the negotiated version is min(peer max, our max). */
    message_type_version,

    /* Acknowledgement of a server command, sent by an aircraft client (FMU)
     * back to the server. Optional capability gated by FSS_FEATURE_COMMAND_ACK;
     * appended last so existing type numbering is unchanged for legacy peers.
     * See todo/17 item 1. */
    message_type_command_ack,
};

/* Outcome carried by a command ack. A command may produce up to two acks with
 * the same acked-command id: an early command_ack_received on receipt, then a
 * terminal outcome once the FMU resolves it. The terminal ack is authoritative.
 * - received:   frame decoded; not yet actioned.
 * - actioned:   the FMU state machine transitioned in response to the command.
 * - superseded: received but deliberately not actioned because a higher-priority
 *               latched state is engaged (the FMU prioritises terminate >
 *               low-battery > comms > command); superseding_state names it.
 * - rejected:   unactionable command (unknown/malformed/invalid). */
using fss_command_ack_outcome = enum fss_command_ack_outcome_e {
    command_ack_received = 0,
    command_ack_actioned = 1,
    command_ack_superseded = 2,
    command_ack_rejected = 3,
};

using fss_asset_command = enum fss_asset_command_e {
    asset_command_unknown,
    /* RTL */
    asset_command_rtl,
    /* Hold */
    asset_command_hold,
    /* Goto (supplied position) */
    asset_command_goto,
    /* Resume own navigation */
    asset_command_resume,
    /* Terminate flight (land or crash at current position) */
    asset_command_terminate,
    /* Disarm (i.e. stop motors) */
    asset_command_disarm,
    /* Adjust altitude (climb or descend to supplied height) */
    asset_command_altitude,
    /* Enter Manual Flight Mode */
    asset_command_manual,
};

class buf_len {
private:
    std::string data{};
public:
    buf_len(const buf_len &bl);
    buf_len(buf_len &&bl) noexcept;
    buf_len();
    buf_len(const char *_data, uint16_t len);
    buf_len(const void *_data, uint16_t len);
    auto operator=(buf_len &&) -> buf_len & = delete;
    virtual ~buf_len();
    auto operator=(const buf_len &other) -> buf_len &;
    auto isValid() -> bool;
    auto addData(const char *new_data, size_t len) -> bool;
    auto addData(const void *new_data, size_t len) -> bool;
    void writeAt(size_t offset, const char *src, size_t len);
    void writeAt(size_t offset, const void *src, size_t len);
    auto getData() -> const char *;
    auto getLength() -> size_t;
};

class fss_message_cb {
private:
    std::shared_ptr<fss_connection> conn;
    mutable std::mutex conn_lock{};
protected:
    void setConnection(std::shared_ptr<fss_connection> t_conn);
    void clearConnection();
public:
    explicit fss_message_cb(std::shared_ptr<fss_connection> t_conn);
    fss_message_cb(const fss_message_cb &from);
    fss_message_cb(fss_message_cb &&) = delete;
    auto operator=(fss_message_cb &) -> fss_message_cb & = delete;
    auto operator=(fss_message_cb &&) -> fss_message_cb & = delete;
    virtual ~fss_message_cb();
    auto operator=(const fss_message_cb &other) -> fss_message_cb &;
    virtual void processMessage(std::shared_ptr<fss_message> message) = 0;
    virtual auto getConnection() -> std::shared_ptr<fss_connection>;
    virtual auto connected() -> bool;
    virtual void disconnect();
    virtual auto sendMsg(const std::shared_ptr<fss_message> &msg) -> bool;
};

class fss_connection {
    static constexpr size_t default_max_queue_size = 1000;
    std::atomic<bool> run{false};
    std::atomic<int> fd{-1};
    std::atomic<uint64_t> last_msg_id{0};
    fss_message_cb *handler{nullptr};
    std::queue<std::shared_ptr<fss_message>> messages{};
    std::thread recv_thread{};
    std::mutex send_lock{};
    std::mutex msg_lock{};
    size_t max_queue_size{default_max_queue_size};
    std::atomic<uint64_t> dropped_messages{0};
    /* Consecutive undecodable frames from the peer; reset by the next
     * successfully decoded message. Drives log throttling in
     * processMessages() — atomic so tests/monitoring can read it from
     * another thread. */
    std::atomic<uint64_t> consecutive_null_msgs{0};
    static constexpr uint64_t null_msg_log_interval = 100;
    /* A peer that streams this many consecutive undecodable frames without a
     * single decodable one in between is compromised or badly broken; close
     * the session rather than logging at line rate forever. Interleaved valid
     * traffic resets the counter, so a version-skewed-but-honest peer sending
     * the odd unknown message type is never disconnected. */
    static constexpr uint64_t null_msg_disconnect_threshold = 1000;
    std::atomic<uint16_t> negotiated_version{FSS_PROTOCOL_VERSION_LEGACY};
    /* Capabilities both peers agreed on at handshake (intersection of the two
     * advertised feature_flags, masked by FSS_SUPPORTED_FEATURES). 0 until the
     * version handshake negotiates it; a legacy peer leaves it 0. */
    std::atomic<uint32_t> negotiated_feature_flags{0};
protected:
    auto recvMsg() -> std::shared_ptr<fss_message>;
    auto getMessageId() -> uint64_t;
    virtual auto sendMsg(const std::shared_ptr<flight_safety_system::transport::buf_len> &bl) -> bool;
    /* Contract: never reports an interrupted call — implementations retry
     * EINTR (plain TCP) / GNUTLS_E_INTERRUPTED (TLS) internally.  Returns
     * -2 when the transport is already closed/unusable, 0 on orderly peer
     * close, and a negative value on hard errors. */
    virtual auto recvBytes(void *bytes, size_t max_bytes) -> ssize_t;
    auto getFd() -> int;
    void setFd(int new_fd);
    void startRecvThread(std::thread t_recv_thread);
    explicit fss_connection(int fd, size_t t_max_queue_size = default_max_queue_size);
public:
    fss_connection();
    static auto create(int fd, size_t t_max_queue_size = default_max_queue_size) -> std::shared_ptr<fss_connection>;
    auto getDroppedMessages() -> uint64_t;
    auto getNullMsgCount() -> uint64_t;
    fss_connection(fss_connection &) = delete;
    fss_connection(fss_connection &&) = delete;
    auto operator=(fss_connection &) -> fss_connection & = delete;
    auto operator=(fss_connection &&) -> fss_connection & = delete;
    virtual ~fss_connection();
    void setHandler(fss_message_cb *cb);
    virtual auto connectTo(const std::string &address, uint16_t port) -> bool;
    auto getNegotiatedVersion() -> uint16_t { return this->negotiated_version.load(); }
    void setNegotiatedVersion(uint16_t v) { this->negotiated_version.store(v); }
    auto getNegotiatedFeatureFlags() -> uint32_t { return this->negotiated_feature_flags.load(); }
    void setNegotiatedFeatureFlags(uint32_t f) { this->negotiated_feature_flags.store(f); }
    /* Sends one message on this connection. NOTE: this mutates the message —
     * it stamps the per-connection sequence id into msg (setId) before packing.
     * Invariant (todo/12 C8): a single fss_message instance must not be sent
     * concurrently on multiple connections; the id stamp would race. The one
     * path that fans a shared instance out — broadcastMsg (one msg to every
     * client) — iterates the connections sequentially on a single thread.
     * sendRTTRequest sends a single message and then reads its assigned id back
     * immediately after this returns, so it too relies on the stamp being
     * synchronous and unraced. (sendSMMSettings builds a fresh message per
     * client, so it never shares an instance.) */
    auto sendMsg(const std::shared_ptr<fss_message> &msg) -> bool;
    auto getMsg() -> std::shared_ptr<fss_message>;
    virtual void processMessages();
    virtual void disconnect();
    virtual auto getClientNames() -> std::list<std::string>;
    virtual auto isPeerCertRevoked(const std::string &) const -> bool { return false; }
};

/* NOTE (todo/12 C7): a listen socket is-a fss_connection only to reuse the fd +
 * recv-thread lifecycle; it inherits sendMsg/getMsg/message-queue members that
 * are meaningless for it. The clean shape is a small fd_owner base shared by
 * sibling fss_listen / fss_connection. Deferred deliberately: it is an ABI break
 * (a -version-info bump — all four libs export this header) not worth doing on
 * its own; fold it into the next transport rework. */
class fss_listen : public fss_connection {
private:
    uint16_t port;
    fss_connect_cb cb;
    static constexpr int default_max_pending_conns = 10;
    int max_pending_connections{default_max_pending_conns};
    /* Connection setup (newConnection + cb) runs on detached worker threads,
     * not inline on the accept thread: for the TLS listener newConnection
     * performs the blocking handshake, so running it inline let one silent
     * peer block all new connections. setup_lock guards the bookkeeping
     * below; the count is bounded so the worker path cannot itself become a
     * thread/memory-exhaustion vector. */
    static constexpr size_t default_max_concurrent_setups = 64;
    std::mutex setup_lock{};
    std::condition_variable setup_cv{};
    size_t active_setups{0};
    bool accepting_setups{true};
    size_t max_concurrent_setups{default_max_concurrent_setups};
    std::atomic<uint64_t> rejected_setups{0};
    /* Spawns the detached worker that runs newConnection()+cb for an admitted
     * fd; the caller has already reserved the slot. May throw std::system_error
     * if the thread cannot be created (caller rolls the reservation back). */
    void startSetupWorker(int t_newfd);
    /* Releases one reserved setup slot and wakes a draining disconnect(). The
     * single place the active_setups decrement + notify lives, so the worker
     * exit path and the thread-creation-failure path stay in lockstep. */
    void releaseSetupSlot();
protected:
    virtual auto newConnection(int fd) -> std::shared_ptr<flight_safety_system::transport::fss_connection>;
    /* Binds, listens, and starts the accept thread. The public constructor
     * calls this; derived classes use the defer_start_t constructor and call
     * it at the END of their own constructor instead, so the accept thread
     * (which virtual-dispatches processMessages/newConnection and reads
     * derived members) can never observe a partially constructed object. */
    auto startListening() -> bool;
    /* Bound on concurrent in-progress connection setups. Derived classes set
     * this before startListening() so the accept thread reads a settled value. */
    void setMaxConcurrentSetups(size_t t_max);
    struct defer_start_t {};
    fss_listen(uint16_t t_port, fss_connect_cb t_cb, defer_start_t);
public:
    fss_listen(uint16_t t_port, fss_connect_cb t_cb);
    fss_listen(fss_listen &) = delete;
    fss_listen(fss_listen &&) = delete;
    auto operator=(fss_listen &) -> fss_listen & = delete;
    auto operator=(fss_listen &&) -> fss_listen & = delete;
    ~fss_listen() override;
    void processMessages() override;
    /* Stops accepting new setups and drains in-flight setup workers before
     * returning, so derived members the workers read stay alive until then. */
    void disconnect() override;
    auto getActiveSetupCount() -> size_t;
    auto getRejectedSetupCount() -> uint64_t;
};

class fss_message {
private:
    uint64_t id;
    fss_message_type type;
protected:
    static auto headerLength() -> size_t;
    virtual void packData(std::shared_ptr<buf_len> bl) = 0;
public:
    explicit fss_message(fss_message_type t_type);
    fss_message(uint64_t t_id, fss_message_type t_type);
    fss_message(fss_message &) = delete;
    fss_message(fss_message &&) = delete;
    auto operator=(fss_message &) -> fss_message & = delete;
    auto operator=(fss_message &&) -> fss_message & = delete;
    virtual ~fss_message();
    void setId(uint64_t t_id);
    auto getId() -> uint64_t;
    auto getSeq() -> uint64_t;
    auto getType() -> fss_message_type;
    virtual auto getLatitude() -> double;
    virtual auto getLongitude() -> double;
    virtual auto getAltitude() -> uint32_t;
    virtual auto getTimeStamp() -> uint64_t;
    virtual auto getPacked() -> std::shared_ptr<buf_len>;
    void createHeader(const std::shared_ptr<buf_len> &bl);
    static void updateSize(const std::shared_ptr<buf_len> &bl);
    static auto decode(const std::shared_ptr<buf_len> &bl) -> std::shared_ptr<fss_message>;
};

class fss_message_closed : public fss_message {
protected:
    void packData(std::shared_ptr<buf_len>) override;
public:
    fss_message_closed();
};

class fss_message_identity : public fss_message {
private:
    std::string name;
protected:
    void unpackData(const std::shared_ptr<buf_len> &bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    explicit fss_message_identity(std::string t_name);
    fss_message_identity(uint64_t t_id, const std::shared_ptr<buf_len> &bl);
    virtual auto getName() -> std::string;
};

class fss_message_rtt_request : public fss_message {
protected:
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_rtt_request();
    fss_message_rtt_request(uint64_t t_id, const std::shared_ptr<buf_len> &bl);
};

class fss_message_rtt_response : public fss_message {
private:
    uint64_t request_id;
    /* The responder's wall-clock reading (ms since epoch) at the moment it
     * built this response. 0 means "not reported" — a legacy peer, or one that
     * has not negotiated the RTT clock-offset capability. Carried as an
     * optional trailing wire field (see todo/17 item 3); the server uses it to
     * estimate the client↔server clock offset that feeds the position
     * staleness gate.
     *
     * The 0 sentinel is lossy: it cannot distinguish "not reported" from a
     * responder whose clock genuinely reads Unix epoch midnight (an unsynced
     * RTC or a simulator stub). That collision degrades safely — the consumer
     * treats epoch-0 as "no measurement", so the offset stays 0 and the
     * staleness gate falls back to a symmetric window; an aircraft with a
     * frozen 1970 clock has no usable offset to feed it anyway. */
    uint64_t client_timestamp{0};
protected:
    void unpackData(const std::shared_ptr<buf_len> &bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    explicit fss_message_rtt_response(uint64_t t_request_id);
    fss_message_rtt_response(uint64_t t_request_id, uint64_t t_client_timestamp);
    fss_message_rtt_response(uint64_t t_id, const std::shared_ptr<buf_len> &bl);
    virtual auto getRequestId() -> uint64_t;
    virtual auto getClientTimestamp() -> uint64_t;
};

class fss_message_position_report : public fss_message {
private:
    double latitude{NAN};
    double longitude{NAN};
    uint32_t altitude{0};
    uint16_t heading{0};
    uint16_t horizontal_velocity{0};
    int16_t vertical_velocity{0};
    std::string callsign{};
    uint32_t icao_address{0};
    uint16_t squawk{0};
    uint64_t timestamp{0};
    uint8_t tslc{0};
    uint16_t flags{0};
    uint8_t altitude_type{0};
    uint8_t emitter_type{0};
protected:
    void unpackData(const std::shared_ptr<buf_len> &bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_position_report(double t_latitude, double t_longitude, uint32_t t_altitude, uint16_t t_heading,
                                uint16_t t_hor_vel, int16_t t_ver_vel, uint32_t t_icao_address, std::string t_callsign,
                                uint16_t t_squawk, uint8_t t_tslc, uint16_t t_flags, uint8_t t_alt_type,
                                uint8_t t_emitter_type, uint64_t t_timestamp);
    fss_message_position_report(uint64_t t_id, const std::shared_ptr<buf_len> &bl);
    auto getLatitude() -> double override;
    auto getLongitude() -> double override;
    auto getAltitude() -> uint32_t override;
    auto getTimeStamp() -> uint64_t override;
    virtual auto getICAOAddress() -> uint32_t;
    virtual auto getHeading() -> uint16_t;
    virtual auto getHorzVel() -> uint16_t;
    virtual auto getVertVel() -> int16_t;
    virtual auto getCallSign() -> std::string;
    virtual auto getSquawk() -> uint16_t;
    virtual auto getTSLC() -> uint8_t;
    virtual auto getFlags() -> uint16_t;
    virtual auto getAltitudeType() -> uint8_t;
    virtual auto getEmitterType() -> uint8_t;
};

class fss_message_system_status : public fss_message {
private:
    uint8_t bat_percent;
    uint32_t mah_used;
    double voltage;
protected:
    void unpackData(const std::shared_ptr<buf_len> &bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_system_status(uint8_t bat_remaining_percent, uint32_t bat_mah_used, double bat_voltage = 0.0);
    fss_message_system_status(uint64_t t_id, const std::shared_ptr<buf_len> &bl);
    virtual auto getBatRemaining() -> uint8_t;
    virtual auto getBatMAHUsed() -> uint32_t;
    virtual auto getBatVoltage() -> double;
};

class fss_message_search_status : public fss_message {
private:
    uint64_t search_id;
    uint64_t point_completed;
    uint64_t points_total;
protected:
    void unpackData(const std::shared_ptr<buf_len> &bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_search_status(uint64_t t_search_id, uint64_t last_point_completed, uint64_t total_search_points);
    fss_message_search_status(uint64_t t_id, const std::shared_ptr<buf_len> &bl);
    virtual auto getSearchId() -> uint64_t;
    virtual auto getSearchCompleted() -> uint64_t;
    virtual auto getSearchTotal() -> uint64_t;
};

class fss_message_asset_command : public fss_message {
private:
    fss_asset_command command;
    double latitude;
    double longitude;
    uint32_t altitude;
    uint64_t timestamp;
protected:
    void unpackData(const std::shared_ptr<buf_len> &bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_asset_command(fss_asset_command t_command, uint64_t t_timestamp);
    fss_message_asset_command(fss_asset_command t_command, uint64_t t_timestamp, double t_latitude, double t_longitude);
    fss_message_asset_command(fss_asset_command t_command, uint64_t t_timestamp, uint32_t t_altitude);
    fss_message_asset_command(uint64_t t_id, const std::shared_ptr<buf_len> &bl);
    virtual auto getCommand() -> fss_asset_command;
    auto getLatitude() -> double override;
    auto getLongitude() -> double override;
    auto getAltitude() -> uint32_t override;
    auto getTimeStamp() -> uint64_t override;
};

class fss_message_smm_settings : public fss_message {
private:
    std::string server_url;
    secure_string username;
    secure_string password;
protected:
    void unpackData(const std::shared_ptr<buf_len> &bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_smm_settings(std::string t_server_url, secure_string t_username, secure_string t_password);
    fss_message_smm_settings(uint64_t t_id, const std::shared_ptr<buf_len> &bl);
    virtual auto getServerURL() -> std::string;
    virtual auto getUsername() -> const secure_string &;
    virtual auto getPassword() -> const secure_string &;
};

class fss_message_server_list : public fss_message {
private:
    std::vector<std::pair<std::string, uint16_t>> servers;
protected:
    void unpackData(const std::shared_ptr<buf_len> &bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_server_list();
    fss_message_server_list(uint64_t t_id, const std::shared_ptr<buf_len> &bl);
    virtual void addServer(const std::string &server, uint16_t port);
    virtual auto getServers() -> std::vector<std::pair<std::string, uint16_t>>;
};

class fss_message_identity_non_aircraft : public fss_message {
private:
    uint64_t capabilities{0};
protected:
    void unpackData(const std::shared_ptr<buf_len> &bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_identity_non_aircraft();
    fss_message_identity_non_aircraft(uint64_t t_id, const std::shared_ptr<buf_len> &bl);
    void addCapability(uint8_t cap_id);
    auto getCapability(uint8_t cap_id) -> bool;
};

class fss_message_identity_required : public fss_message {
private:
protected:
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_identity_required();
    fss_message_identity_required(uint64_t t_id, const std::shared_ptr<buf_len> &bl);
};

class fss_message_version : public fss_message {
private:
    uint16_t protocol_version{FSS_PROTOCOL_VERSION};
    uint16_t min_supported_version{FSS_PROTOCOL_MIN_VERSION};
    /* A default-constructed version message (the one each peer sends at
     * handshake) advertises exactly the capabilities this build implements. */
    uint32_t feature_flags{FSS_SUPPORTED_FEATURES};
protected:
    void unpackData(const std::shared_ptr<buf_len> &bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_version();
    fss_message_version(uint16_t t_version, uint16_t t_min_version, uint32_t t_flags);
    fss_message_version(uint64_t t_id, const std::shared_ptr<buf_len> &bl);
    auto getProtocolVersion() const -> uint16_t { return this->protocol_version; }
    auto getMinSupportedVersion() const -> uint16_t { return this->min_supported_version; }
    auto getFeatureFlags() const -> uint32_t { return this->feature_flags; }
};

class fss_message_command_ack : public fss_message {
private:
    /* The header id of the asset_command being acked, echoed back so the server
     * can match the ack to the specific command it sent rather than "last
     * command seen" (commands carry a per-connection monotonic header id). */
    uint64_t acked_command_id{0};
    /* The fss_asset_command value being acked. Redundant with the command the
     * server already holds under acked_command_id, but cheap and useful for
     * cross-checking and human-readable storage. */
    uint8_t command{asset_command_unknown};
    uint8_t outcome{command_ack_received};
    /* The fss_asset_command-domain value of the higher-priority state that
     * blocked actioning; asset_command_unknown (0) unless outcome is
     * command_ack_superseded. */
    uint8_t superseding_state{asset_command_unknown};
    /* The responder's wall-clock reading (ms since epoch) when it built the ack. */
    uint64_t timestamp{0};
protected:
    void unpackData(const std::shared_ptr<buf_len> &bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_command_ack(uint64_t t_acked_command_id, fss_asset_command t_command, fss_command_ack_outcome t_outcome,
                            uint64_t t_timestamp);
    fss_message_command_ack(uint64_t t_acked_command_id, fss_asset_command t_command, fss_command_ack_outcome t_outcome,
                            fss_asset_command t_superseding_state, uint64_t t_timestamp);
    fss_message_command_ack(uint64_t t_id, const std::shared_ptr<buf_len> &bl);
    virtual auto getAckedCommandId() -> uint64_t;
    virtual auto getCommand() -> fss_asset_command;
    virtual auto getOutcome() -> fss_command_ack_outcome;
    virtual auto getSupersedingState() -> fss_asset_command;
    auto getTimeStamp() -> uint64_t override;
};
} // namespace transport
} // namespace flight_safety_system
