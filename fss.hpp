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

namespace flight_safety_system {
class fss;
class fss_connection;
class fss_listen;
class fss_message;

typedef bool (*fss_message_cb)(fss_connection *conn, fss_message *message);
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
    buf_len() : data(NULL), length(0) {};
    buf_len(char *data, uint16_t len) : data(data), length(len) {};
    ~buf_len() { free(this->data); };
    bool isValid() { return this->data != NULL; };
    bool addData(const char *data, uint16_t len)
    {
        char *new_data = (char *)realloc(this->data, this->length + len);
        if (new_data == NULL)
        {
            return false;
        }
        this->data = new_data;
        memcpy(this->data + this->length, data, len);
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

class fss_connection {
private:
protected:
    int fd;
    uint64_t last_msg_id;
    fss_message_cb handler;
    std::queue<fss_message *> messages;
    std::thread recv_thread;
    fss_message *recvMsg();
public:
    fss_connection();
    fss_connection(int fd);
    virtual ~fss_connection();
    void setHandler(fss_message_cb cb);
    bool connect_to(std::string address, uint16_t port);
    uint64_t getMessageId();
    bool sendMsg(fss_message *msg);
    bool sendMsg(buf_len &bl);
    fss_message *getMsg();
    virtual void processMessages();
};

class fss_listen : public fss_connection {
private:
    uint16_t port;
    bool startListening();
    fss_connect_cb cb;
public:
    fss_listen(int port, fss_connect_cb cb) : fss_connection(), port(port), cb(cb) {
        this->startListening();
    };
    virtual void processMessages();
};

class fss_message {
private:
    uint64_t id;
    fss_message_type type;
protected:
    size_t headerLength();
    virtual void unpackData(buf_len &bl) = 0;
    virtual void packData(buf_len &bl) = 0;
public:
    fss_message(uint64_t id, fss_message_type type) : id(id), type(type) {};
    virtual ~fss_message() {};
    uint64_t getId() { return this->id; };
    fss_message_type getType() { return this->type; };
    virtual double getLatitude() { return NAN; };
    virtual double getLongitude() { return NAN; };
    virtual uint32_t getAltitude() { return 0; };
    virtual uint64_t getTimeStamp() { return 0; };
    virtual buf_len getPacked();
    void createHeader(buf_len &bl);
    void updateSize(buf_len &bl);
    static fss_message *decode(buf_len &bl);
};

class fss_message_closed: public fss_message {
protected:
    virtual void unpackData(buf_len &bl);
    virtual void packData(buf_len &bl);
public:
    fss_message_closed() : fss_message(0, message_type_closed) {};
};

class fss_message_identity : public fss_message {
private:
    std::string name;
protected:
    virtual void unpackData(buf_len &bl);
    virtual void packData(buf_len &bl);
public:
    fss_message_identity(uint64_t id, std::string name) : fss_message(id, message_type_identity), name(name) {};
    fss_message_identity(uint64_t id, buf_len &bl) : fss_message(id, message_type_identity) { this->unpackData(bl); };
    virtual std::string getName() { return this->name; };
};

class fss_message_rtt_request : public fss_message {
protected:
    virtual void unpackData(buf_len &bl);
    virtual void packData(buf_len &bl);
public:
    fss_message_rtt_request(uint64_t id) : fss_message(id, message_type_rtt_request) {};
    fss_message_rtt_request(uint64_t id, buf_len &bl) : fss_message(id, message_type_rtt_request) {};
    virtual buf_len getPacked();
};

class fss_message_rtt_response : public fss_message {
private:
    uint64_t request_id;
protected:
    virtual void unpackData(buf_len &bl);
    virtual void packData(buf_len &bl);
public:
    fss_message_rtt_response(uint64_t id, uint64_t request_id) : fss_message(id, message_type_rtt_response), request_id(request_id) {};
    fss_message_rtt_response(uint64_t id, buf_len &bl) : fss_message(id, message_type_rtt_response) { this->unpackData(bl); };
    virtual uint64_t getRequestId() { return this->request_id; };
};

class fss_message_position_report : public fss_message {
private:
    double latitude;
    double longitude;
    uint32_t altitude;
    uint64_t timestamp;
protected:
    virtual void unpackData(buf_len &bl);
    virtual void packData(buf_len &bl);
public:
    fss_message_position_report(uint64_t id, double latitude, double longitude, uint32_t altitude, uint64_t timestamp) : fss_message(id, message_type_position_report), latitude(latitude), longitude(longitude), altitude(altitude), timestamp(timestamp) {};
    fss_message_position_report(uint64_t id, buf_len &bl) : fss_message(id, message_type_position_report) { this->unpackData(bl); };
    virtual double getLatitude() { return this->latitude; };
    virtual double getLongitude() { return this->longitude; };
    virtual uint32_t getAltitude() { return this->altitude; };
    virtual uint64_t getTimeStamp() { return this->timestamp; };
};

class fss_message_system_status: public fss_message {
private:
    uint8_t bat_percent;
    uint32_t mah_used;
protected:
    virtual void unpackData(buf_len &bl);
    virtual void packData(buf_len &bl);
public:
    fss_message_system_status(uint64_t id, uint8_t bat_remaining_percent, uint8_t mah_used) : fss_message(id, message_type_system_status), bat_percent(bat_remaining_percent), mah_used(mah_used) {};
    fss_message_system_status(uint64_t id, buf_len &bl) : fss_message(id, message_type_system_status) { this->unpackData(bl); };
    virtual uint8_t getBatRemaining() { return this->bat_percent; };
    virtual uint32_t getBatMAHUsed() { return this->mah_used; };
};

class fss_message_search_status: public fss_message {
private:
    uint64_t search_id;
    uint64_t point_completed;
    uint64_t points_total;
protected:
    virtual void unpackData(buf_len &bl);
    virtual void packData(buf_len &bl);
public:
    fss_message_search_status(uint64_t id, uint64_t search_id, uint64_t last_point_completed, uint64_t total_search_points) : fss_message(id, message_type_search_status), search_id(search_id), point_completed(last_point_completed), points_total(total_search_points) {};
    fss_message_search_status(uint64_t id, buf_len &bl) : fss_message(id, message_type_search_status) { this->unpackData(bl); };
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
    virtual void unpackData(buf_len &bl);
    virtual void packData(buf_len &bl);
public:
    fss_message_asset_command(uint64_t id, fss_asset_command command, uint64_t timestamp) : fss_message(id, message_type_command), command(command) {};
    fss_message_asset_command(uint64_t id, fss_asset_command command, uint64_t timestamp, double latitude, double longitude) : fss_message(id, message_type_command), command(command), latitude(latitude), longitude(longitude) {};
    fss_message_asset_command(uint64_t id, fss_asset_command command, uint64_t timestamp, uint32_t altitude) : fss_message(id, message_type_command), command(command), altitude(altitude) {};
    fss_message_asset_command(uint64_t id, buf_len &bl) : fss_message(id, message_type_command) { this->unpackData(bl); };
    virtual fss_asset_command getCommand() { return this->command; };
    virtual double getLatitude() { return this->latitude; };
    virtual double getLongitude() { return this->longitude; };
    virtual uint32_t getAltitude() { return this->altitude; };
    virtual uint64_t getTimeStamp() { return this->timestamp; };
};

class fss_message_smm_settings: public fss_message {
private:
    std::string server_url;
    std::string username;
    std::string password;
protected:
    virtual void unpackData(buf_len &bl);
    virtual void packData(buf_len &bl);
public:
    fss_message_smm_settings(uint64_t id, std::string server_url, std::string username, std::string password) : fss_message(id, message_type_smm_settings), server_url(server_url), username(username), password(password) {};
    fss_message_smm_settings(uint64_t id, buf_len &bl) : fss_message(id, message_type_smm_settings) { this->unpackData(bl); };
    virtual std::string getServerURL() { return this->server_url; };
    virtual std::string getUsername() { return this->username; };
    virtual std::string getPassword() { return this->password; };
};

class fss_message_server_list: public fss_message {
private:
    std::vector<std::pair<std::string, uint16_t>> servers;
protected:
    virtual void unpackData(buf_len &bl);
    virtual void packData(buf_len &bl);
public:
    fss_message_server_list(uint64_t id) : fss_message(id, message_type_server_list) {};
    fss_message_server_list(uint64_t id, buf_len &bl) : fss_message(id, message_type_server_list) { this->unpackData(bl); };
    virtual void addServer(std::string server, uint16_t port) { this->servers.push_back(std::pair<std::string, uint16_t>(server, port)); };
    virtual std::vector<std::pair<std::string, uint16_t>> getServers() { return this->servers; };
};

class fss {
private:
    std::string name;
public:
    fss(std::string name);
    fss_connection *connect(std::string name, uint16_t port);
    std::string getName() { return this->name; };
};

}

#endif
