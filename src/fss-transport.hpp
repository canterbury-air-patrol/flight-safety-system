#pragma once

#include <memory>
#include <sys/types.h>
#include <thread>

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
    char *data{nullptr};
    uint16_t length{0};
public:
    buf_len(const buf_len &bl) : data(static_cast<char *>(malloc(bl.length))), length(bl.length)
    {
        memcpy (this->data, bl.data, this->length);
    }
    buf_len(buf_len &&bl) : data(bl.data), length(bl.length)
    {
        bl.data = nullptr;
        bl.length = 0;
    }
    buf_len() = default;
    buf_len(const char *_data, uint16_t len) : data(static_cast<char *>(malloc(len))), length(len) {
        memcpy (this->data, _data, this->length);
    };
    auto operator=(buf_len &&) -> buf_len& = delete;
    ~buf_len() { free(this->data); };
    auto operator=(const buf_len &other) -> buf_len &
    {
        if (this != &other)
        {
            free(this->data);
            this->length = other.length;
            this->data = (char *) malloc(this->length);
            memcpy(this->data, other.data, this->length);
        }
        return *this;
    }
    auto isValid() -> bool { return this->data != nullptr; };
    auto addData(const char *new_data, uint16_t len) -> bool
    {
        char *tmp_data = (char *)realloc(this->data, this->length + len);
        if (tmp_data == nullptr)
        {
            return false;
        }
        this->data = tmp_data;
        memcpy(this->data + this->length, new_data, len);
        this->length += len;
        return true;
    }
    auto getData() -> const char *
    {
        return this->data;
    }
    auto getLength() -> size_t
    {
        return this->length;
    }
};

class fss_message_cb {
private:
    std::shared_ptr<fss_connection> conn;
protected:
    void setConnection(std::shared_ptr<fss_connection> t_conn) { this->conn = t_conn; };
    void clearConnection() { this->conn = nullptr; };
public:
    explicit fss_message_cb(std::shared_ptr<fss_connection> t_conn) : conn(std::move(t_conn)) {};
    fss_message_cb(const fss_message_cb &from) : conn(from.conn) {};
    fss_message_cb(fss_message_cb&&) = delete;
    auto operator=(fss_message_cb &) -> fss_message_cb& = delete;
    auto operator=(fss_message_cb &&) -> fss_message_cb& = delete;
    virtual ~fss_message_cb();
    auto operator=(const fss_message_cb& other) -> fss_message_cb&
    {
        if (this != &other)
        {
            this->conn = other.conn;
        }
        return *this;
    }
    virtual void processMessage(std::shared_ptr<fss_message> message) = 0;
    virtual auto getConnection() -> std::shared_ptr<fss_connection> { return this->conn; };
    virtual auto connected() -> bool { return this->conn != nullptr; };
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
    auto getFd() -> int { return this->fd; };
    void setFd(int new_fd) { this->fd = new_fd; };
    void startRecvThread(std::thread t_recv_thread) { this->recv_thread = std::move(t_recv_thread); };
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
    fss_listen(uint16_t t_port, fss_connect_cb t_cb) : fss_connection(), port(t_port), cb(t_cb) {
        this->startListening();
    };
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
    explicit fss_message(fss_message_type t_type) : id(0), type(t_type) {};
    fss_message(uint64_t t_id, fss_message_type t_type) : id(t_id), type(t_type) {};
    fss_message(fss_message &) = delete;
    fss_message(fss_message &&) = delete;
    auto operator=(fss_message &) -> fss_message& = delete;
    auto operator=(fss_message &&) -> fss_message& = delete;
    virtual ~fss_message() = default;
    void setId(uint64_t t_id) { this->id = t_id; };
    auto getId() -> uint64_t { return this->id; };
    auto getType() -> fss_message_type { return this->type; };
    virtual auto getLatitude() -> double { return NAN; };
    virtual auto getLongitude() -> double { return NAN; };
    virtual auto getAltitude() -> uint32_t { return 0; };
    virtual auto getTimeStamp() -> uint64_t { return 0; };
    virtual auto getPacked() -> std::shared_ptr<buf_len>;
    void createHeader(std::shared_ptr<buf_len> bl);
    void updateSize(std::shared_ptr<buf_len> bl);
    static auto decode(std::shared_ptr<buf_len> bl) -> std::shared_ptr<fss_message>;
};

class fss_message_closed: public fss_message {
protected:
    void packData(std::shared_ptr<buf_len>) override {};
public:
    fss_message_closed() : fss_message(message_type_closed) {};
};

class fss_message_identity : public fss_message {
private:
    std::string name;
protected:
    void unpackData(std::shared_ptr<buf_len> bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    explicit fss_message_identity(std::string t_name) : fss_message(message_type_identity), name(std::move(t_name)) {};
    fss_message_identity(uint64_t t_id, std::shared_ptr<buf_len> bl) : fss_message(t_id, message_type_identity), name() { this->unpackData(bl); };
    virtual auto getName() -> std::string { return this->name; };
};

class fss_message_rtt_request : public fss_message {
protected:
    void packData(std::shared_ptr<buf_len> bl __attribute__((unused))) override {};
public:
    fss_message_rtt_request() : fss_message(message_type_rtt_request) {};
    fss_message_rtt_request(uint64_t t_id, std::shared_ptr<buf_len> bl __attribute__((unused))) : fss_message(t_id, message_type_rtt_request) {};
};

class fss_message_rtt_response : public fss_message {
private:
    uint64_t request_id;
protected:
    void unpackData(std::shared_ptr<buf_len> bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    explicit fss_message_rtt_response(uint64_t t_request_id) : fss_message(message_type_rtt_response), request_id(t_request_id) {};
    fss_message_rtt_response(uint64_t t_id, std::shared_ptr<buf_len> bl) : fss_message(t_id, message_type_rtt_response), request_id(0) { this->unpackData(bl); };
    virtual auto getRequestId() -> uint64_t { return this->request_id; };
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
    void unpackData(std::shared_ptr<buf_len> bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_position_report(double t_latitude, double t_longitude, uint32_t t_altitude,
                                uint16_t t_heading, uint16_t t_hor_vel, int16_t t_ver_vel,
                                uint32_t t_icao_address, std::string t_callsign,
                                uint16_t t_squawk, uint8_t t_tslc, uint16_t t_flags, uint8_t t_alt_type,
                                uint8_t t_emitter_type, uint64_t t_timestamp) :
        fss_message(message_type_position_report), latitude(t_latitude), longitude(t_longitude),
        altitude(t_altitude), heading(t_heading), horizontal_velocity(t_hor_vel), vertical_velocity(t_ver_vel),
        callsign(std::move(t_callsign)), icao_address(t_icao_address), squawk(t_squawk), timestamp(t_timestamp),
        tslc(t_tslc), flags(t_flags), altitude_type(t_alt_type), emitter_type(t_emitter_type) {};
    fss_message_position_report(uint64_t t_id, std::shared_ptr<buf_len> bl) : fss_message(t_id, message_type_position_report) { this->unpackData(bl); };
    auto getLatitude() -> double override { return this->latitude; };
    auto getLongitude() -> double override { return this->longitude; };
    auto getAltitude() -> uint32_t override { return this->altitude; };
    auto getTimeStamp() -> uint64_t override { return this->timestamp; };
    virtual auto getICAOAddress() -> uint32_t { return this->icao_address; };
    virtual auto getHeading() -> uint16_t { return this->heading; };
    virtual auto getHorzVel() -> uint16_t { return this->horizontal_velocity; };
    virtual auto getVertVel() -> int16_t { return this->vertical_velocity; };
    virtual auto getCallSign() -> std::string { return this->callsign; };
    virtual auto getSquawk() -> uint16_t { return this->squawk; };
    virtual auto getFlags() -> uint16_t { return this->flags; };
    virtual auto getAltitudeType() -> uint8_t { return this->altitude_type; };
    virtual auto getEmitterType() -> uint8_t { return this->emitter_type; };
};

class fss_message_system_status: public fss_message {
private:
    uint8_t bat_percent;
    uint32_t mah_used;
protected:
    void unpackData(std::shared_ptr<buf_len> bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_system_status(uint8_t bat_remaining_percent, uint32_t bat_mah_used) : fss_message(message_type_system_status), bat_percent(bat_remaining_percent), mah_used(bat_mah_used) {};
    fss_message_system_status(uint64_t t_id, std::shared_ptr<buf_len> bl) : fss_message(t_id, message_type_system_status), bat_percent(0), mah_used(0) { this->unpackData(bl); };
    virtual auto getBatRemaining() -> uint8_t { return this->bat_percent; };
    virtual auto getBatMAHUsed() -> uint32_t { return this->mah_used; };
};

class fss_message_search_status: public fss_message {
private:
    uint64_t search_id;
    uint64_t point_completed;
    uint64_t points_total;
protected:
    void unpackData(std::shared_ptr<buf_len> bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_search_status(uint64_t t_search_id, uint64_t last_point_completed, uint64_t total_search_points) : fss_message(message_type_search_status), search_id(t_search_id), point_completed(last_point_completed), points_total(total_search_points) {};
    fss_message_search_status(uint64_t t_id, std::shared_ptr<buf_len> bl) : fss_message(t_id, message_type_search_status), search_id(0), point_completed(0), points_total(0) { this->unpackData(bl); };
    virtual auto getSearchId() -> uint64_t { return this->search_id; };
    virtual auto getSearchCompleted() -> uint64_t { return this->point_completed; };
    virtual auto getSearchTotal() -> uint64_t { return this->points_total; };
};

class fss_message_asset_command: public fss_message {
private:
    fss_asset_command command;
    double latitude;
    double longitude;
    uint32_t altitude;
    uint64_t timestamp;
protected:
    void unpackData(std::shared_ptr<buf_len> bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_asset_command(fss_asset_command t_command, uint64_t t_timestamp) : fss_message(message_type_command), command(t_command), latitude(NAN), longitude(NAN), altitude(0), timestamp(t_timestamp) {};
    fss_message_asset_command(fss_asset_command t_command, uint64_t t_timestamp, double t_latitude, double t_longitude) : fss_message(message_type_command), command(t_command), latitude(t_latitude), longitude(t_longitude), altitude(0), timestamp(t_timestamp) {};
    fss_message_asset_command(fss_asset_command t_command, uint64_t t_timestamp, uint32_t t_altitude) : fss_message(message_type_command), command(t_command), latitude(NAN), longitude(NAN), altitude(t_altitude), timestamp(t_timestamp) {};
    fss_message_asset_command(uint64_t t_id, std::shared_ptr<buf_len> bl) : fss_message(t_id, message_type_command), command(asset_command_unknown), latitude(NAN), longitude(NAN), altitude(0), timestamp(0) { this->unpackData(bl); };
    virtual auto getCommand() -> fss_asset_command { return this->command; };
    auto getLatitude() -> double override { return this->latitude; };
    auto getLongitude() -> double override { return this->longitude; };
    auto getAltitude() -> uint32_t override { return this->altitude; };
    auto getTimeStamp() -> uint64_t override { return this->timestamp; };
};

class fss_message_smm_settings: public fss_message {
private:
    std::string server_url;
    std::string username;
    std::string password;
protected:
    void unpackData(std::shared_ptr<buf_len> bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_smm_settings(std::string t_server_url, std::string t_username, std::string t_password) : fss_message(message_type_smm_settings), server_url(std::move(t_server_url)), username(std::move(t_username)), password(std::move(t_password)) {};
    fss_message_smm_settings(uint64_t t_id, std::shared_ptr<buf_len> bl) : fss_message(t_id, message_type_smm_settings), server_url(), username(), password() { this->unpackData(bl); };
    virtual auto getServerURL() -> std::string { return this->server_url; };
    virtual auto getUsername() -> std::string { return this->username; };
    virtual auto getPassword() -> std::string { return this->password; };
};

class fss_message_server_list: public fss_message {
private:
    std::vector<std::pair<std::string, uint16_t>> servers;
protected:
    void unpackData(std::shared_ptr<buf_len> bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_server_list() : fss_message(message_type_server_list), servers() {};
    fss_message_server_list(uint64_t t_id, std::shared_ptr<buf_len> bl) : fss_message(t_id, message_type_server_list), servers() { this->unpackData(bl); };
    virtual void addServer(const std::string &server, uint16_t port) { this->servers.emplace_back(server, port); };
    virtual auto getServers() -> std::vector<std::pair<std::string, uint16_t>> { return this->servers; };
};

class fss_message_identity_non_aircraft: public fss_message {
private:
    uint64_t capabilities{0};
protected:
    void unpackData(std::shared_ptr<buf_len> bl);
    void packData(std::shared_ptr<buf_len> bl) override;
public:
    fss_message_identity_non_aircraft() : fss_message(message_type_identity_non_aircraft) {};
    fss_message_identity_non_aircraft(uint64_t t_id, std::shared_ptr<buf_len> bl) : fss_message(t_id, message_type_identity_non_aircraft) { this->unpackData(bl); };
    void addCapability(uint8_t cap_id);
    auto getCapability(uint8_t cap_id) -> bool;
};
} // namespace transport
} // namespace flight_safety_system
