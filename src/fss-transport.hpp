#pragma once

#include <memory>
#include <sys/types.h>
#include <thread>
#include <list>

#include "fss.hpp"

namespace flight_safety_system {

namespace transport {
class fss_connection;
class fss_listen;
class fss_message;

using  fss_connect_cb = bool (*)(std::shared_ptr<fss_connection> conn);

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
    auto operator=(buf_len &&) -> buf_len& = delete;
    virtual ~buf_len();
    auto operator=(const buf_len &other) -> buf_len &;
    auto isValid() -> bool;
    auto addData(const char *new_data, uint16_t len) -> bool;
    auto getData() -> const char *;
    auto getLength() -> size_t;
};

class fss_message_cb {
private:
    std::shared_ptr<fss_connection> conn;
protected:
    void setConnection(std::shared_ptr<fss_connection> t_conn);
    void clearConnection();
public:
    explicit fss_message_cb(std::shared_ptr<fss_connection> t_conn);
    fss_message_cb(const fss_message_cb &from);
    fss_message_cb(fss_message_cb&&) = delete;
    auto operator=(fss_message_cb &) -> fss_message_cb& = delete;
    auto operator=(fss_message_cb &&) -> fss_message_cb& = delete;
    virtual ~fss_message_cb();
    auto operator=(const fss_message_cb& other) -> fss_message_cb&;
    virtual void processMessage(std::shared_ptr<fss_message> message) = 0;
    virtual auto getConnection() -> std::shared_ptr<fss_connection>;
    virtual auto connected() -> bool;
    virtual void disconnect();
    virtual auto sendMsg(const std::shared_ptr<fss_message> &msg) -> bool;
};

class fss_connection {
    bool run{false};
    int fd{-1};
    uint64_t last_msg_id{0};
    fss_message_cb *handler{nullptr};
    std::queue<std::shared_ptr<fss_message>> messages{};
    std::thread recv_thread{};
    std::mutex send_lock{};
protected:
    auto recvMsg() -> std::shared_ptr<fss_message>;
    auto getMessageId() -> uint64_t;
    virtual auto sendMsg(const std::shared_ptr<flight_safety_system::transport::buf_len> &bl) -> bool;
    virtual auto recvBytes(void *bytes, size_t max_bytes) -> ssize_t;
    auto getFd() -> int;
    void setFd(int new_fd);
    void startRecvThread(std::thread t_recv_thread);
public:
    fss_connection() = default;
    explicit fss_connection(int fd);
    fss_connection(fss_connection&) = delete;
    fss_connection(fss_connection&&) = delete;
    auto operator=(fss_connection &) -> fss_connection& = delete;
    auto operator=(fss_connection &&) -> fss_connection& = delete;
    virtual ~fss_connection();
    void setHandler(fss_message_cb *cb);
    virtual auto connectTo(const std::string &address, uint16_t port) -> bool;
    auto sendMsg(const std::shared_ptr<fss_message> &msg) -> bool;
    auto getMsg() -> std::shared_ptr<fss_message>;
    virtual void processMessages();
    void disconnect();
    virtual auto getClientNames() -> std::list<std::string>;
};

class fss_listen : public fss_connection {
private:
    uint16_t port;
    auto startListening() -> bool;
    fss_connect_cb cb;
    static constexpr int default_max_pending_conns = 10;
    int max_pending_connections{default_max_pending_conns};
protected:
    virtual auto newConnection(int fd) -> std::shared_ptr<flight_safety_system::transport::fss_connection>;
public:
    fss_listen(uint16_t t_port, fss_connect_cb t_cb);
    fss_listen(fss_listen &) = delete;
    fss_listen(fss_listen &&) = delete;
    auto operator=(fss_listen &) -> fss_listen& = delete;
    auto operator=(fss_listen &&) -> fss_listen& = delete;
    ~fss_listen() override;
    void processMessages() override;
};

class fss_message {
private:
    uint64_t id;
    fss_message_type type;
protected:
    auto headerLength() -> size_t;
    virtual void packData(std::shared_ptr<buf_len> bl) = 0;
public:
    explicit fss_message(fss_message_type t_type);
    fss_message(uint64_t t_id, fss_message_type t_type);
    fss_message(fss_message &) = delete;
    fss_message(fss_message &&) = delete;
    auto operator=(fss_message &) -> fss_message& = delete;
    auto operator=(fss_message &&) -> fss_message& = delete;
    virtual ~fss_message() = default;
    void setId(uint64_t t_id);
    auto getId() -> uint64_t;
    auto getType() -> fss_message_type;
    virtual auto getLatitude() -> double;
    virtual auto getLongitude() -> double;
    virtual auto getAltitude() -> uint32_t;
    virtual auto getTimeStamp() -> uint64_t;
    virtual auto getPacked() -> std::shared_ptr<buf_len>;
    void createHeader(const std::shared_ptr<buf_len> &bl);
    void updateSize(const std::shared_ptr<buf_len> &bl);
    static auto decode(const std::shared_ptr<buf_len> &bl) -> std::shared_ptr<fss_message>;
};

class fss_message_closed: public fss_message {
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
protected:
    void unpackData(const std::shared_ptr<buf_len> &bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    explicit fss_message_rtt_response(uint64_t t_request_id);
    fss_message_rtt_response(uint64_t t_id, const std::shared_ptr<buf_len> &bl);
    virtual auto getRequestId() -> uint64_t;
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
    fss_message_position_report(double t_latitude, double t_longitude, uint32_t t_altitude,
                                uint16_t t_heading, uint16_t t_hor_vel, int16_t t_ver_vel,
                                uint32_t t_icao_address, std::string t_callsign,
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

class fss_message_system_status: public fss_message {
private:
    uint8_t bat_percent;
    uint32_t mah_used;
    double voltage;
protected:
    void unpackData(const std::shared_ptr<buf_len> &bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_system_status(uint8_t bat_remaining_percent, uint32_t bat_mah_used, double bat_voltage=0.0);
    fss_message_system_status(uint64_t t_id, const std::shared_ptr<buf_len> &bl);
    virtual auto getBatRemaining() -> uint8_t;
    virtual auto getBatMAHUsed() -> uint32_t;
    virtual auto getBatVoltage() -> double;
};

class fss_message_search_status: public fss_message {
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

class fss_message_asset_command: public fss_message {
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

class fss_message_smm_settings: public fss_message {
private:
    std::string server_url;
    std::string username;
    std::string password;
protected:
    void unpackData(const std::shared_ptr<buf_len> &bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_smm_settings(std::string t_server_url, std::string t_username, std::string t_password);
    fss_message_smm_settings(uint64_t t_id, const std::shared_ptr<buf_len> &bl);
    virtual auto getServerURL() -> std::string;
    virtual auto getUsername() -> std::string;
    virtual auto getPassword() -> std::string;
};

class fss_message_server_list: public fss_message {
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

class fss_message_identity_non_aircraft: public fss_message {
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
} // namespace transport
} // namespace flight_safety_system
