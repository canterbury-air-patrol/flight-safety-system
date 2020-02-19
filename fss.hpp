#ifndef __FLIGHT_SAFETY_SYSTEM_HPP__
#define __FLIGHT_SAFETY_SYSTEM_HPP__

#include <string>
#include <cstring>
#include <queue>
#include <vector>
#include <cmath>
#include <thread>

#include <iostream>

#include <stdbool.h>

#include <sys/socket.h>
#include <netinet/in.h>

namespace flight_safety_system {
class fss;
class fss_connection;
class fss_listen;
class fss_message;

typedef bool (*fss_connect_cb)(fss_connection *conn);

typedef enum {
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
} fss_message_type;

typedef enum {
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
} fss_asset_command;

class buf_len {
private:
    char *data;
    uint16_t length;
public:
    buf_len(buf_len &bl) : data((char *)malloc(bl.length)), length(bl.length)
    {
        memcpy (this->data, bl.data, this->length);
    }
    buf_len() : data(NULL), length(0) {};
    buf_len(char *_data, uint16_t len) : data(_data), length(len) {};
    ~buf_len() { free(this->data); };
    buf_len &operator=(const buf_len &other)
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
    bool isValid() { return this->data != NULL; };
    bool addData(const char *new_data, uint16_t len)
    {
        char *tmp_data = (char *)realloc(this->data, this->length + len);
        if (tmp_data == NULL)
        {
            return false;
        }
        this->data = tmp_data;
        memcpy(this->data + this->length, new_data, len);
        this->length += len;
        return true;
    }
    char *getData()
    {
        return this->data;
    }
    size_t getLength()
    {
        return this->length;
    }
};

class fss_message_cb {
protected:
    fss_connection *conn;
public:
    explicit fss_message_cb(fss_connection *t_conn) : conn(t_conn) {};
    fss_message_cb(fss_message_cb &from) : conn(from.conn) {};
    virtual ~fss_message_cb() {};
    fss_message_cb& operator=(const fss_message_cb& other)
    {
        if (this != &other)
        {
            this->conn = other.conn;
        }
        return *this;
    }
    virtual void processMessage(fss_message *message) = 0;
};

class fss_connection {
private:
protected:
    int fd;
    uint64_t last_msg_id;
    fss_message_cb *handler;
    std::queue<fss_message *> messages;
    std::thread recv_thread;
    fss_message *recvMsg();
    fss_connection& operator=(const fss_connection& other) { return *this; };
    fss_connection(const fss_connection &from) : fd(-1), last_msg_id(0), handler(nullptr), messages(), recv_thread() {};
public:
    fss_connection() : fd(socket(PF_INET6, SOCK_STREAM, IPPROTO_TCP)), last_msg_id(0), handler(nullptr), messages(), recv_thread() {};
    explicit fss_connection(int fd);
    virtual ~fss_connection();
    void setHandler(fss_message_cb *cb);
    bool connect_to(const std::string &address, uint16_t port);
    uint64_t getMessageId();
    bool sendMsg(fss_message *msg);
    bool sendMsg(buf_len *bl);
    fss_message *getMsg();
    virtual void processMessages();
};

class fss_listen : public fss_connection {
private:
    uint16_t port;
    bool startListening();
    fss_connect_cb cb;
public:
    fss_listen(uint16_t t_port, fss_connect_cb t_cb) : fss_connection(), port(t_port), cb(t_cb) {
        this->startListening();
    };
    virtual void processMessages() override;
};

class fss_message {
private:
    uint64_t id;
    fss_message_type type;
protected:
    size_t headerLength();
    virtual void unpackData(buf_len *bl) = 0;
    virtual void packData(buf_len *bl) = 0;
public:
    fss_message(uint64_t t_id, fss_message_type t_type) : id(t_id), type(t_type) {};
    virtual ~fss_message() {};
    uint64_t getId() { return this->id; };
    fss_message_type getType() { return this->type; };
    virtual double getLatitude() { return NAN; };
    virtual double getLongitude() { return NAN; };
    virtual uint32_t getAltitude() { return 0; };
    virtual uint64_t getTimeStamp() { return 0; };
    virtual buf_len *getPacked();
    void createHeader(buf_len *bl);
    void updateSize(buf_len *bl);
    static fss_message *decode(buf_len *bl);
};

class fss_message_closed: public fss_message {
protected:
    virtual void unpackData(buf_len *bl) override;
    virtual void packData(buf_len *bl) override;
public:
    fss_message_closed() : fss_message(0, message_type_closed) {};
};

class fss_message_identity : public fss_message {
private:
    std::string name;
protected:
    virtual void unpackData(buf_len *bl) override;
    virtual void packData(buf_len *bl) override;
public:
    fss_message_identity(uint64_t t_id, const std::string &t_name) : fss_message(t_id, message_type_identity), name(t_name) {};
    fss_message_identity(uint64_t t_id, buf_len *bl) : fss_message(t_id, message_type_identity), name() { this->unpackData(bl); };
    virtual std::string getName() { return this->name; };
};

class fss_message_rtt_request : public fss_message {
protected:
    virtual void unpackData(buf_len *bl) override;
    virtual void packData(buf_len *bl) override;
public:
    explicit fss_message_rtt_request(uint64_t t_id) : fss_message(t_id, message_type_rtt_request) {};
    fss_message_rtt_request(uint64_t t_id, buf_len *bl) : fss_message(t_id, message_type_rtt_request) {};
};

class fss_message_rtt_response : public fss_message {
private:
    uint64_t request_id;
protected:
    virtual void unpackData(buf_len *bl) override;
    virtual void packData(buf_len *bl) override;
public:
    fss_message_rtt_response(uint64_t t_id, uint64_t t_request_id) : fss_message(t_id, message_type_rtt_response), request_id(t_request_id) {};
    fss_message_rtt_response(uint64_t t_id, buf_len *bl) : fss_message(t_id, message_type_rtt_response), request_id(0) { this->unpackData(bl); };
    virtual uint64_t getRequestId() { return this->request_id; };
};

class fss_message_position_report : public fss_message {
private:
    double latitude;
    double longitude;
    uint32_t altitude;
    uint64_t timestamp;
protected:
    virtual void unpackData(buf_len *bl) override;
    virtual void packData(buf_len *bl) override;
public:
    fss_message_position_report(uint64_t t_id, double t_latitude, double t_longitude, uint32_t t_altitude, uint64_t t_timestamp) : fss_message(t_id, message_type_position_report), latitude(t_latitude), longitude(t_longitude), altitude(t_altitude), timestamp(t_timestamp) {};
    fss_message_position_report(uint64_t t_id, buf_len *bl) : fss_message(t_id, message_type_position_report), latitude(NAN), longitude(NAN), altitude(0), timestamp(0) { this->unpackData(bl); };
    virtual double getLatitude() override { return this->latitude; };
    virtual double getLongitude() override { return this->longitude; };
    virtual uint32_t getAltitude() override { return this->altitude; };
    virtual uint64_t getTimeStamp() override { return this->timestamp; };
};

class fss_message_system_status: public fss_message {
private:
    uint8_t bat_percent;
    uint32_t mah_used;
protected:
    virtual void unpackData(buf_len *bl) override;
    virtual void packData(buf_len *bl) override;
public:
    fss_message_system_status(uint64_t t_id, uint8_t bat_remaining_percent, uint32_t bat_mah_used) : fss_message(t_id, message_type_system_status), bat_percent(bat_remaining_percent), mah_used(bat_mah_used) {};
    fss_message_system_status(uint64_t t_id, buf_len *bl) : fss_message(t_id, message_type_system_status), bat_percent(0), mah_used(0) { this->unpackData(bl); };
    virtual uint8_t getBatRemaining() { return this->bat_percent; };
    virtual uint32_t getBatMAHUsed() { return this->mah_used; };
};

class fss_message_search_status: public fss_message {
private:
    uint64_t search_id;
    uint64_t point_completed;
    uint64_t points_total;
protected:
    virtual void unpackData(buf_len *bl) override;
    virtual void packData(buf_len *bl) override;
public:
    fss_message_search_status(uint64_t t_id, uint64_t t_search_id, uint64_t last_point_completed, uint64_t total_search_points) : fss_message(t_id, message_type_search_status), search_id(t_search_id), point_completed(last_point_completed), points_total(total_search_points) {};
    fss_message_search_status(uint64_t t_id, buf_len *bl) : fss_message(t_id, message_type_search_status), search_id(0), point_completed(0), points_total(0) { this->unpackData(bl); };
    virtual uint64_t getSearchId() { return this->search_id; };
    virtual uint64_t getSearchCompleted() { return this->point_completed; };
    virtual uint64_t getSearchTotal() { return this->points_total; };
};

class fss_message_asset_command: public fss_message {
private:
    fss_asset_command command;
    double latitude;
    double longitude;
    uint32_t altitude;
    uint64_t timestamp;
protected:
    virtual void unpackData(buf_len *bl) override;
    virtual void packData(buf_len *bl) override;
public:
    fss_message_asset_command(uint64_t t_id, fss_asset_command t_command, uint64_t t_timestamp) : fss_message(t_id, message_type_command), command(t_command), latitude(NAN), longitude(NAN), altitude(0), timestamp(0) {};
    fss_message_asset_command(uint64_t t_id, fss_asset_command t_command, uint64_t t_timestamp, double t_latitude, double t_longitude) : fss_message(t_id, message_type_command), command(t_command), latitude(t_latitude), longitude(t_longitude), altitude(0), timestamp(0) {};
    fss_message_asset_command(uint64_t t_id, fss_asset_command t_command, uint64_t t_timestamp, uint32_t t_altitude) : fss_message(t_id, message_type_command), command(t_command), latitude(NAN), longitude(NAN), altitude(t_altitude), timestamp(0) {};
    fss_message_asset_command(uint64_t t_id, buf_len *bl) : fss_message(t_id, message_type_command), command(asset_command_unknown), latitude(NAN), longitude(NAN), altitude(0), timestamp(0) { this->unpackData(bl); };
    virtual fss_asset_command getCommand() { return this->command; };
    virtual double getLatitude() override { return this->latitude; };
    virtual double getLongitude() override { return this->longitude; };
    virtual uint32_t getAltitude() override { return this->altitude; };
    virtual uint64_t getTimeStamp() override { return this->timestamp; };
};

class fss_message_smm_settings: public fss_message {
private:
    std::string server_url;
    std::string username;
    std::string password;
protected:
    virtual void unpackData(buf_len *bl) override;
    virtual void packData(buf_len *bl) override;
public:
    fss_message_smm_settings(uint64_t t_id, const std::string &t_server_url, const std::string &t_username, const std::string &t_password) : fss_message(t_id, message_type_smm_settings), server_url(t_server_url), username(t_username), password(t_password) {};
    fss_message_smm_settings(uint64_t t_id, buf_len *bl) : fss_message(t_id, message_type_smm_settings), server_url(), username(), password() { this->unpackData(bl); };
    virtual std::string getServerURL() { return this->server_url; };
    virtual std::string getUsername() { return this->username; };
    virtual std::string getPassword() { return this->password; };
};

class fss_message_server_list: public fss_message {
private:
    std::vector<std::pair<std::string, uint16_t>> servers;
protected:
    virtual void unpackData(buf_len *bl) override;
    virtual void packData(buf_len *bl) override;
public:
    explicit fss_message_server_list(uint64_t t_id) : fss_message(t_id, message_type_server_list), servers() {};
    fss_message_server_list(uint64_t t_id, buf_len *bl) : fss_message(t_id, message_type_server_list), servers() { this->unpackData(bl); };
    virtual void addServer(std::string server, uint16_t port) { this->servers.push_back(std::pair<std::string, uint16_t>(server, port)); };
    virtual std::vector<std::pair<std::string, uint16_t>> getServers() { return this->servers; };
};

class fss {
private:
    std::string name;
public:
    explicit fss(const std::string &t_name) : name(t_name) {};
    fss_connection *connect(std::string name, uint16_t port);
    std::string getName() { return this->name; };
};

uint64_t fss_current_timestamp();
}

#endif
