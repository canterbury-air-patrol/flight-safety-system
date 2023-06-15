#include "fss-transport.hpp"

#include <arpa/inet.h>
#include <memory>
#if __APPLE__
/* Apple already has these defines */
#else
#include <endian.h>
#define ntohll(x) be64toh(x)
#define htonll(x) htobe64(x)
#endif

void
packString(const std::shared_ptr<flight_safety_system::transport::buf_len> &bl, const std::string &val)
{
    size_t str_len = val.length();
    uint16_t len = htons(str_len);
    bl->addData((char *)&len, sizeof(uint16_t));
    bl->addData(val.c_str(), str_len);
    /* align to 8-byte boundary (will add 1 to 8 bytes on '\0') */
    uint64_t empty = 0;
    bl->addData((char *)&empty, sizeof(uint64_t) - (bl->getLength() % sizeof(uint64_t)));
}

flight_safety_system::transport::buf_len::buf_len(const buf_len &bl) = default;
flight_safety_system::transport::buf_len::buf_len(buf_len &&bl) noexcept = default;
flight_safety_system::transport::buf_len::buf_len() = default;

flight_safety_system::transport::buf_len::buf_len(const char *_data, uint16_t len) : data(_data, len)
{
}

flight_safety_system::transport::buf_len::~buf_len() = default;

auto
flight_safety_system::transport::buf_len::operator=(const buf_len &other) -> buf_len &
{
    if (this != &other)
    {
        this->data = other.data;
    }
    return *this;
}

auto
flight_safety_system::transport::buf_len::isValid() -> bool
{
    return this->data.length() != 0;
}

auto
flight_safety_system::transport::buf_len::addData(const char *new_data, uint16_t len) -> bool
{
    this->data.append(new_data, len);
    return true;
}

auto
flight_safety_system::transport::buf_len::getData() -> const char *
{
    return this->data.c_str();
}

auto
flight_safety_system::transport::buf_len::getLength() -> size_t
{
    return this->data.length();
}

flight_safety_system::transport::fss_message_cb::fss_message_cb(std::shared_ptr<fss_connection> t_conn) : conn(std::move(t_conn))
{
}

flight_safety_system::transport::fss_message_cb::fss_message_cb(const fss_message_cb &from) = default;

void
flight_safety_system::transport::fss_message_cb::setConnection(std::shared_ptr<fss_connection> t_conn)
{
    this->conn = std::move(t_conn);
}

void flight_safety_system::transport::fss_message_cb::clearConnection()
{
    this->conn = nullptr;
}

auto flight_safety_system::transport::fss_message_cb::operator=(const flight_safety_system::transport::fss_message_cb& other) -> fss_message_cb&
{
    if (this != &other)
    {
        this->conn = other.conn;
    }
    return *this;
}

auto flight_safety_system::transport::fss_message_cb::getConnection() -> std::shared_ptr<fss_connection>
{
    return this->conn;
}
auto flight_safety_system::transport::fss_message_cb::connected() -> bool
{
    return this->conn != nullptr;
}

void
flight_safety_system::transport::fss_message_cb::disconnect()
{
    if (this->conn != nullptr)
    {
        this->conn->disconnect();
        this->conn = nullptr;
    }
}

auto
flight_safety_system::transport::fss_message_cb::sendMsg(const std::shared_ptr<fss_message> &msg) -> bool
{
    if (this->conn != nullptr)
    {
        return this->conn->sendMsg(msg);
    }
    return false;
}

flight_safety_system::transport::fss_message::fss_message(fss_message_type t_type) : id(0), type(t_type)
{
}

flight_safety_system::transport::fss_message::fss_message(uint64_t t_id, fss_message_type t_type) : id(t_id), type(t_type)
{
}

flight_safety_system::transport::fss_message::~fss_message() = default;

auto
flight_safety_system::transport::fss_message::headerLength() -> size_t
{
    return sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint64_t);
}

void
flight_safety_system::transport::fss_message::setId(uint64_t t_id)
{
    this->id = t_id;
}
auto
flight_safety_system::transport::fss_message::getId() -> uint64_t
{
    return this->id;
}
auto
flight_safety_system::transport::fss_message::getType() -> fss_message_type
{
    return this->type;
}
auto
flight_safety_system::transport::fss_message::getLatitude() -> double
{
    return NAN;
}
auto
flight_safety_system::transport::fss_message::getLongitude() -> double
{
    return NAN;
}
auto
flight_safety_system::transport::fss_message::getAltitude() -> uint32_t
{
    return 0;
}
auto
flight_safety_system::transport::fss_message::getTimeStamp() -> uint64_t
{
    return 0;
}

void
flight_safety_system::transport::fss_message::createHeader(const std::shared_ptr<buf_len> &bl)
{
    /* Make space for length, type, id */
    size_t length = this->headerLength();
    char *data = (char *)malloc(length);
    memset(data, '0', length);
    /* Set the type */
    *(uint16_t *)(data + sizeof(uint16_t)) = htons(this->getType());
    /* Set the id */
    *(uint64_t *)(data + sizeof(uint16_t) + sizeof(uint16_t)) = htonll(this->getId());
    bl->addData(data, length);
    free(data);
}

void
flight_safety_system::transport::fss_message::updateSize(const std::shared_ptr<buf_len> &bl)
{
    uint16_t length = bl->getLength();
    if (length > sizeof(uint16_t))
    {
        /* Set the length */
        if (length % sizeof(uint64_t) != 0)
        {
            uint64_t blank = 0;
            bl->addData((char *)&blank, sizeof(uint64_t) - (length % sizeof(uint64_t)));
        }
        *(uint16_t *)(bl->getData()) = htons(length);
    }
}

auto
flight_safety_system::transport::fss_message::getPacked() -> std::shared_ptr<buf_len>
{
    auto bl = std::make_shared<buf_len>();

    this->createHeader(bl);

    this->packData(bl);

    this->updateSize(bl);

    return bl;
}

void
flight_safety_system::transport::fss_message_closed::packData(std::shared_ptr<buf_len> bl __attribute__((unused)))
{
}

flight_safety_system::transport::fss_message_closed::fss_message_closed() : fss_message(message_type_closed)
{
}

flight_safety_system::transport::fss_message_identity::fss_message_identity(std::string t_name) : fss_message(message_type_identity), name(std::move(t_name))
{
}

void
flight_safety_system::transport::fss_message_identity::packData(std::shared_ptr<buf_len> bl)
{
    bl->addData(this->name.c_str(), this->name.length());
}

void
flight_safety_system::transport::fss_message_identity::unpackData(const std::shared_ptr<buf_len> &bl)
{
    size_t offset = this->headerLength();
    const char *data = bl->getData();
    size_t length = bl->getLength();
    char *name_n = (char *)calloc(1, (length - offset) + 1);
    memcpy(name_n, data + offset, length - offset);
    name_n[length - offset] = '\0';
    this->name = std::string(name_n);
    free (name_n);
}

flight_safety_system::transport::fss_message_identity::fss_message_identity(uint64_t t_id, const std::shared_ptr<buf_len> &bl) : fss_message(t_id, message_type_identity), name()
{
    this->unpackData(bl);
}

auto
flight_safety_system::transport::fss_message_identity::getName() -> std::string
{
    return this->name;
}


flight_safety_system::transport::fss_message_rtt_request::fss_message_rtt_request() : fss_message(message_type_rtt_request)
{
}

flight_safety_system::transport::fss_message_rtt_request::fss_message_rtt_request(uint64_t t_id, const std::shared_ptr<buf_len> &bl __attribute__((unused))) : fss_message(t_id, message_type_rtt_request)
{
}

void
flight_safety_system::transport::fss_message_rtt_request::packData(std::shared_ptr<buf_len> bl __attribute__((unused)))
{
}

flight_safety_system::transport::fss_message_rtt_response::fss_message_rtt_response(uint64_t t_request_id) : fss_message(message_type_rtt_response), request_id(t_request_id)
{
}

flight_safety_system::transport::fss_message_rtt_response::fss_message_rtt_response(uint64_t t_id, const std::shared_ptr<buf_len> &bl) : fss_message(t_id, message_type_rtt_response), request_id(0)
{
    this->unpackData(bl);
}

void
flight_safety_system::transport::fss_message_rtt_response::packData(std::shared_ptr<buf_len> bl)
{
    uint64_t data = htonll(this->request_id);
    bl->addData((char *)&data, sizeof(uint64_t));
}

void
flight_safety_system::transport::fss_message_rtt_response::unpackData(const std::shared_ptr<buf_len> &bl)
{
    size_t offset = this->headerLength();
    const char *data = bl->getData();
    size_t length = bl->getLength();
    if (length - offset >= sizeof(uint64_t))
    {
        this->request_id = ntohll(*(uint64_t *)(data + offset));
    }
    else
    {
        this->request_id = 0;
    }
}

auto
flight_safety_system::transport::fss_message_rtt_response::getRequestId() -> uint64_t
{
    return this->request_id;
}

flight_safety_system::transport::fss_message_position_report::fss_message_position_report(double t_latitude, double t_longitude, uint32_t t_altitude,
                                                                                          uint16_t t_heading, uint16_t t_hor_vel, int16_t t_ver_vel,
                                                                                          uint32_t t_icao_address, std::string t_callsign,
                                                                                          uint16_t t_squawk, uint8_t t_tslc, uint16_t t_flags, uint8_t t_alt_type,
                                                                                          uint8_t t_emitter_type, uint64_t t_timestamp) :
    fss_message(message_type_position_report), latitude(t_latitude), longitude(t_longitude),
    altitude(t_altitude), heading(t_heading), horizontal_velocity(t_hor_vel), vertical_velocity(t_ver_vel),
    callsign(std::move(t_callsign)), icao_address(t_icao_address), squawk(t_squawk), timestamp(t_timestamp),
    tslc(t_tslc), flags(t_flags), altitude_type(t_alt_type), emitter_type(t_emitter_type)
{
}


flight_safety_system::transport::fss_message_position_report::fss_message_position_report(uint64_t t_id, const std::shared_ptr<buf_len> &bl) : fss_message(t_id, message_type_position_report)
{
    this->unpackData(bl);
}

constexpr float flt_to_int = 0.000001;
void
flight_safety_system::transport::fss_message_position_report::packData(std::shared_ptr<buf_len> bl)
{
    uint64_t ts = htonll(this->getTimeStamp());
    /* Convert the lat/long to fixed decimal for transport */
    int32_t lat = htonl((int32_t) (this->getLatitude() / flt_to_int));
    int32_t lng = htonl((int32_t) (this->getLongitude() / flt_to_int));
    uint32_t alt = htonl(this->getAltitude());
    uint32_t icao_id = htonl(this->getICAOAddress());
    uint16_t head = htons(this->getHeading());
    uint16_t hor_vel = htons(this->getHorzVel());
    int16_t ver_vel = htons(this->getVertVel());
    uint16_t squawk_code = htons(this->getSquawk());
    uint16_t enc_flags = htons(this->getFlags());

    bl->addData((char *)&ts, sizeof(uint64_t));
    bl->addData((char *)&lat, sizeof(int32_t));
    bl->addData((char *)&lng, sizeof(int32_t));
    bl->addData((char *)&alt, sizeof(uint32_t));
    bl->addData((char *)&icao_id, sizeof(uint32_t));
    bl->addData((char *)&head, sizeof(uint16_t));
    bl->addData((char *)&hor_vel, sizeof(uint16_t));
    bl->addData((char *)&ver_vel, sizeof(int16_t));
    bl->addData((char *)&squawk_code, sizeof(uint16_t));
    packString(bl, this->getCallSign());
    bl->addData((char *)&enc_flags, sizeof(uint16_t));
    bl->addData((char *)&this->altitude_type, sizeof(uint8_t));
    bl->addData((char *)&this->emitter_type, sizeof(uint8_t));
}

void
flight_safety_system::transport::fss_message_position_report::unpackData(const std::shared_ptr<buf_len> &bl)
{
    size_t offset = this->headerLength();
    const char *data = bl->getData();
    size_t length = bl->getLength();
    int32_t lat = 0;
    int32_t lng = 0;
    this->altitude = 0;
    this->timestamp = 0;
    if (length - offset >= sizeof(uint64_t))
    {
        this->timestamp = ntohll(*(uint64_t *)(data + offset));
        offset += sizeof(uint64_t);
    }
    if (length - offset >= sizeof(int32_t))
    {
        lat = ntohl(*(int32_t *)(data + offset));
        offset += sizeof(int32_t);
        this->latitude = ((double)lat) * flt_to_int;
    }
    if (length - offset >= sizeof(int32_t))
    {
        lng = ntohl(*(int32_t *)(data + offset));
        offset += sizeof(int32_t);
        this->longitude = ((double)lng) * flt_to_int;
    }
    if (length - offset >= sizeof(uint32_t))
    {
        this->altitude = ntohl(*(uint32_t *)(data + offset));
        offset += sizeof(uint32_t);
    }
    if (length - offset >= sizeof(uint32_t))
    {
        this->icao_address = ntohl(*(uint32_t *)(data + offset));
        offset += sizeof(uint32_t);
    }
    if (length - offset >= sizeof(uint16_t))
    {
        this->heading = ntohs(*(uint16_t *)(data + offset));
        offset += sizeof(uint16_t);
    }
    if (length - offset >= sizeof(uint16_t))
    {
        this->horizontal_velocity = ntohs(*(uint16_t *)(data + offset));
        offset += sizeof(uint16_t);
    }
    if (length - offset >= sizeof(int16_t))
    {
        this->vertical_velocity = ntohs(*(int16_t *)(data + offset));
        offset += sizeof(int16_t);
    }
    if (length - offset >= sizeof(uint16_t))
    {
        this->squawk = ntohs(*(uint16_t *)(data + offset));
        offset += sizeof(uint16_t);
    }
    if (length - offset >= sizeof(uint16_t))
    {
        uint16_t len = ntohs(*(uint16_t *)(data + offset));
        offset += sizeof(uint16_t);
        this->callsign.assign((char *)(data + offset), len);
        offset += len;
        offset += (sizeof(uint64_t) - offset % sizeof(uint64_t));
    }
    if (length - offset >= sizeof(uint16_t))
    {
        this->flags = ntohs(*(uint16_t *)(data + offset));
        offset += sizeof(uint16_t);
    }
    if (length - offset >= sizeof(uint8_t))
    {
        this->altitude_type = (*(uint8_t *)(data + offset));
        offset += sizeof(uint8_t);
    }
    if (length - offset >= sizeof(uint8_t))
    {
        this->emitter_type = (*(uint8_t *)(data + offset));
        offset += sizeof(uint8_t);
    }
}

auto
flight_safety_system::transport::fss_message_position_report::getLatitude() -> double
{
    return this->latitude;
}
auto
flight_safety_system::transport::fss_message_position_report::getLongitude() -> double
{
    return this->longitude;
}
auto
flight_safety_system::transport::fss_message_position_report::getAltitude() -> uint32_t
{
    return this->altitude;
}
auto
flight_safety_system::transport::fss_message_position_report::getTimeStamp() -> uint64_t
{
    return this->timestamp;
}
auto
flight_safety_system::transport::fss_message_position_report::getICAOAddress() -> uint32_t
{
    return this->icao_address;
}
auto
flight_safety_system::transport::fss_message_position_report::getHeading() -> uint16_t
{
    return this->heading;
}
auto
flight_safety_system::transport::fss_message_position_report::getHorzVel() -> uint16_t
{
    return this->horizontal_velocity;
}
auto
flight_safety_system::transport::fss_message_position_report::getVertVel() -> int16_t
{
    return this->vertical_velocity;
}
auto
flight_safety_system::transport::fss_message_position_report::getCallSign() -> std::string
{
    return this->callsign;
}
auto
flight_safety_system::transport::fss_message_position_report::getSquawk() -> uint16_t
{
    return this->squawk;
}
auto
flight_safety_system::transport::fss_message_position_report::getTSLC() -> uint8_t
{
    return this->tslc;
}
auto
flight_safety_system::transport::fss_message_position_report::getFlags() -> uint16_t
{
    return this->flags;
}
auto
flight_safety_system::transport::fss_message_position_report::getAltitudeType() -> uint8_t
{
    return this->altitude_type;
}
auto
flight_safety_system::transport::fss_message_position_report::getEmitterType() -> uint8_t
{
    return this->emitter_type;
}

flight_safety_system::transport::fss_message_system_status::fss_message_system_status(uint8_t bat_remaining_percent, uint32_t bat_mah_used, double bat_voltage) : fss_message(message_type_system_status), bat_percent(bat_remaining_percent), mah_used(bat_mah_used), voltage(bat_voltage)
{
}

flight_safety_system::transport::fss_message_system_status::fss_message_system_status(uint64_t t_id, const std::shared_ptr<buf_len> &bl) : fss_message(t_id, message_type_system_status), bat_percent(0), mah_used(0), voltage(0.0)
{
    this->unpackData(bl);
}

void
flight_safety_system::transport::fss_message_system_status::packData(std::shared_ptr<buf_len> bl)
{
    uint8_t bat_percent_n = this->getBatRemaining();
    uint32_t mah_used_n = htonl(this->getBatMAHUsed());
    uint32_t voltage_n = htonl((int32_t) (this->getBatVoltage() / flt_to_int));

    bl->addData((char *)&bat_percent_n, sizeof(uint8_t));
    bl->addData((char *)&mah_used_n, sizeof(uint32_t));
    bl->addData((char *)&voltage_n, sizeof(uint32_t));
}

void
flight_safety_system::transport::fss_message_system_status::unpackData(const std::shared_ptr<buf_len> &bl)
{
    size_t offset = this->headerLength();
    const char *data = bl->getData();
    size_t length = bl->getLength();
    this->bat_percent = 0;
    this->mah_used = 0;
    if (length - offset >= (sizeof(uint8_t) + sizeof(uint32_t)))
    {
        this->bat_percent = *(uint8_t *)(data + offset);
        offset += sizeof(uint8_t);
        this->mah_used = ntohl(*(uint32_t *)(data + offset));
        offset += sizeof(uint32_t);
    }
    if (length - offset >= sizeof(int32_t))
    {
        uint32_t voltage_n = ntohl(*(int32_t *)(data + offset));
        offset += sizeof(int32_t);
        this->voltage = ((double)voltage_n) * flt_to_int;
    }
}

auto
flight_safety_system::transport::fss_message_system_status::getBatRemaining() -> uint8_t
{
    return this->bat_percent;
}
auto
flight_safety_system::transport::fss_message_system_status::getBatMAHUsed() -> uint32_t
{
    return this->mah_used;
}
auto flight_safety_system::transport::fss_message_system_status::getBatVoltage() -> double
{
    return this->voltage;
}

flight_safety_system::transport::fss_message_search_status::fss_message_search_status(uint64_t t_search_id, uint64_t last_point_completed, uint64_t total_search_points) : fss_message(message_type_search_status), search_id(t_search_id), point_completed(last_point_completed), points_total(total_search_points)
{
}

flight_safety_system::transport::fss_message_search_status::fss_message_search_status(uint64_t t_id, const std::shared_ptr<buf_len> &bl) : fss_message(t_id, message_type_search_status), search_id(0), point_completed(0), points_total(0)
{
    this->unpackData(bl);
}

void
flight_safety_system::transport::fss_message_search_status::packData(std::shared_ptr<buf_len> bl)
{
    uint64_t search_id_n = htonll(this->getSearchId());
    uint64_t point_completed_n = htonll(this->getSearchCompleted());
    uint64_t points_total_n = htonll(this->getSearchTotal());
    bl->addData((char *)&search_id_n, sizeof(uint64_t));
    bl->addData((char *)&point_completed_n, sizeof(uint64_t));
    bl->addData((char *)&points_total_n, sizeof(uint64_t));
}

void
flight_safety_system::transport::fss_message_search_status::unpackData(const std::shared_ptr<buf_len> &bl)
{
    size_t offset = this->headerLength();
    const char *data = bl->getData();
    size_t length = bl->getLength();
    this->search_id = 0;
    this->point_completed = 0;
    this->points_total = 0;
    if (length - offset >= (sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint64_t)))
    {
        this->search_id = ntohll(*(uint64_t *)(data + offset));
        offset += sizeof(uint64_t);
        this->point_completed = ntohll(*(uint64_t *)(data + offset));
        offset += sizeof(uint64_t);
        this->points_total = ntohll(*(uint64_t *)(data + offset));
    }
}

auto
flight_safety_system::transport::fss_message_search_status::getSearchId() -> uint64_t
{
    return this->search_id;
}
auto
flight_safety_system::transport::fss_message_search_status::getSearchCompleted() -> uint64_t
{
    return this->point_completed;
}
auto
flight_safety_system::transport::fss_message_search_status::getSearchTotal() -> uint64_t
{
    return this->points_total;
}

flight_safety_system::transport::fss_message_asset_command::fss_message_asset_command(fss_asset_command t_command, uint64_t t_timestamp) : fss_message(message_type_command), command(t_command), latitude(NAN), longitude(NAN), altitude(0), timestamp(t_timestamp)
{
}

flight_safety_system::transport::fss_message_asset_command::fss_message_asset_command(fss_asset_command t_command, uint64_t t_timestamp, double t_latitude, double t_longitude) : fss_message(message_type_command), command(t_command), latitude(t_latitude), longitude(t_longitude), altitude(0), timestamp(t_timestamp)
{
}

flight_safety_system::transport::fss_message_asset_command::fss_message_asset_command(fss_asset_command t_command, uint64_t t_timestamp, uint32_t t_altitude) : fss_message(message_type_command), command(t_command), latitude(NAN), longitude(NAN), altitude(t_altitude), timestamp(t_timestamp)
{
}

flight_safety_system::transport::fss_message_asset_command::fss_message_asset_command(uint64_t t_id, const std::shared_ptr<buf_len> &bl) : fss_message(t_id, message_type_command), command(asset_command_unknown), latitude(NAN), longitude(NAN), altitude(0), timestamp(0)
{
    this->unpackData(bl);
}

void
flight_safety_system::transport::fss_message_asset_command::packData(std::shared_ptr<buf_len> bl)
{
    uint64_t ts = htonll(this->getTimeStamp());
    /* Convert the lat/long to fixed decimal for transport */
    int32_t lat = htonl((int32_t) (this->getLatitude() / flt_to_int));
    int32_t lng = htonl((int32_t) (this->getLongitude() / flt_to_int));
    uint32_t alt = htonl(this->getAltitude());
    auto cmd = (uint8_t) this->getCommand();
    bl->addData((char *)&ts, sizeof(uint64_t));
    bl->addData((char *)&lat, sizeof(int32_t));
    bl->addData((char *)&lng, sizeof(int32_t));
    bl->addData((char *)&alt, sizeof(uint32_t));
    bl->addData((char *)&cmd, sizeof(uint8_t));
}

void
flight_safety_system::transport::fss_message_asset_command::unpackData(const std::shared_ptr<buf_len> &bl)
{
    size_t offset = this->headerLength();
    const char *data = bl->getData();
    size_t length = bl->getLength();
    int32_t lat = 0;
    int32_t lng = 0;
    this->altitude = 0;
    this->timestamp = 0;
    if (length - offset >= (sizeof(uint64_t) + sizeof(int32_t) + sizeof(int32_t) + sizeof(uint32_t) + sizeof(uint8_t)))
    {
        this->timestamp = ntohll(*(uint64_t *)(data + offset));
        offset += sizeof(uint64_t);
        lat = ntohl(*(int32_t *)(data + offset));
        offset += sizeof(int32_t);
        lng = ntohl(*(int32_t *)(data + offset));
        offset += sizeof(int32_t);
        this->altitude = ntohl(*(uint32_t *)(data + offset));
        offset += sizeof(uint32_t);
        this->command = (fss_asset_command) *(uint8_t *)(data + offset);
    }
    this->latitude = lat * flt_to_int;
    this->longitude = lng * flt_to_int;
}

auto
flight_safety_system::transport::fss_message_asset_command::getCommand() -> fss_asset_command
{
    return this->command;
}
auto
flight_safety_system::transport::fss_message_asset_command::getLatitude() -> double
{
    return this->latitude;
}
auto
flight_safety_system::transport::fss_message_asset_command::getLongitude() -> double
{
    return this->longitude;
}
auto
flight_safety_system::transport::fss_message_asset_command::getAltitude() -> uint32_t
{
    return this->altitude;
}
auto
flight_safety_system::transport::fss_message_asset_command::getTimeStamp() -> uint64_t
{
    return this->timestamp;
}

void
flight_safety_system::transport::fss_message_smm_settings::packData(std::shared_ptr<buf_len> bl)
{
    packString(bl, this->getServerURL());
    packString(bl, this->getUsername());
    packString(bl, this->getPassword());
}

void
flight_safety_system::transport::fss_message_smm_settings::unpackData(const std::shared_ptr<buf_len> &bl)
{
    size_t offset = this->headerLength();
    const char *data = bl->getData();
    size_t length = bl->getLength();
    if (length - offset >= sizeof(uint16_t))
    {
        uint16_t len = ntohs(*(uint16_t *)(data + offset));
        offset += sizeof(uint16_t);
        this->server_url.assign((char *)(data + offset), len);
        offset += len;
        offset += (sizeof(uint64_t) - offset % sizeof(uint64_t));
    }
    if (length - offset >= sizeof(uint16_t))
    {
        uint16_t len = ntohs(*(uint16_t *)(data + offset));
        offset += sizeof(uint16_t);
        this->username.assign((char *)(data + offset), len);
        offset += len;
        offset += (sizeof(uint64_t) - offset % sizeof(uint64_t));
    }
    if (length - offset >= sizeof(uint16_t))
    {
        uint16_t len = ntohs(*(uint16_t *)(data + offset));
        offset += sizeof(uint16_t);
        this->password.assign((char *)(data + offset), len);
    }
}

flight_safety_system::transport::fss_message_smm_settings::fss_message_smm_settings(std::string t_server_url, std::string t_username, std::string t_password) : fss_message(message_type_smm_settings), server_url(std::move(t_server_url)), username(std::move(t_username)), password(std::move(t_password))
{
}

flight_safety_system::transport::fss_message_smm_settings::fss_message_smm_settings(uint64_t t_id, const std::shared_ptr<buf_len> &bl) : fss_message(t_id, message_type_smm_settings), server_url(), username(), password()
{
    this->unpackData(bl);
}

auto
flight_safety_system::transport::fss_message_smm_settings::getServerURL() -> std::string
{
    return this->server_url;
}

auto
flight_safety_system::transport::fss_message_smm_settings::getUsername() -> std::string
{
    return this->username;
}

auto
flight_safety_system::transport::fss_message_smm_settings::getPassword() -> std::string
{
    return this->password;
}

void
packServer(const std::shared_ptr<flight_safety_system::transport::buf_len> &bl, const std::pair<std::string, uint16_t> &server)
{
    uint16_t port = htons(server.second);
    bl->addData((char *)&port, sizeof(port));
    packString(bl, server.first);
}

void
flight_safety_system::transport::fss_message_server_list::packData(std::shared_ptr<buf_len> bl)
{
    for(auto server : this->servers)
    {
        packServer(bl, server);
    }
}

void
flight_safety_system::transport::fss_message_server_list::unpackData(const std::shared_ptr<buf_len> &bl)
{
    size_t offset = this->headerLength();
    const char *data = bl->getData();
    size_t length = bl->getLength();
    while (length - offset >= (sizeof(uint16_t) + sizeof(uint16_t)))
    {
        uint16_t port = ntohs(*(uint16_t *)(data + offset));
        offset += sizeof(uint16_t);
        uint16_t len = ntohs(*(uint16_t *)(data + offset));
        offset += sizeof(uint16_t);
        std::string server_addr;
        server_addr.assign((char *)(data + offset), len);
        this->servers.emplace_back(server_addr, port);
        offset += len;
        offset += (sizeof(uint64_t) - offset % sizeof(uint64_t));
    }
}

flight_safety_system::transport::fss_message_server_list::fss_message_server_list() : fss_message(message_type_server_list), servers()
{
}

flight_safety_system::transport::fss_message_server_list::fss_message_server_list(uint64_t t_id, const std::shared_ptr<buf_len> &bl) : fss_message(t_id, message_type_server_list), servers()
{
    this->unpackData(bl);
}

void
flight_safety_system::transport::fss_message_server_list::addServer(const std::string &server, uint16_t port)
{
    this->servers.emplace_back(server, port);
}

auto
flight_safety_system::transport::fss_message_server_list::getServers() -> std::vector<std::pair<std::string, uint16_t>>
{
    return this->servers;
}


void
flight_safety_system::transport::fss_message_identity_non_aircraft::packData(std::shared_ptr<buf_len> bl)
{
    uint64_t caps = htonll(this->capabilities);
    bl->addData((char *)&caps, sizeof(uint64_t));
}

void
flight_safety_system::transport::fss_message_identity_non_aircraft::unpackData(const std::shared_ptr<buf_len> &bl)
{
    size_t offset = this->headerLength();
    const char *data = bl->getData();
    size_t length = bl->getLength();
    if (length - offset >= (sizeof(uint64_t)))
    {
        uint64_t caps = ntohll(*(uint64_t *)(data + offset));
        this->capabilities = caps;
    }
}

flight_safety_system::transport::fss_message_identity_non_aircraft::fss_message_identity_non_aircraft() : fss_message(message_type_identity_non_aircraft)
{
}

flight_safety_system::transport::fss_message_identity_non_aircraft::fss_message_identity_non_aircraft(uint64_t t_id, const std::shared_ptr<buf_len> &bl) : fss_message(t_id, message_type_identity_non_aircraft)
{
    this->unpackData(bl);
}

void
flight_safety_system::transport::fss_message_identity_non_aircraft::addCapability(uint8_t cap_id)
{
    this->capabilities |= ((uint64_t)(1) << cap_id);
}

auto
flight_safety_system::transport::fss_message_identity_non_aircraft::getCapability(uint8_t cap_id) -> bool
{
    return !!(this->capabilities & ((uint64_t)(1) << cap_id));
}

void
flight_safety_system::transport::fss_message_identity_required::packData(std::shared_ptr<buf_len> bl __attribute__((unused)))
{
}

flight_safety_system::transport::fss_message_identity_required::fss_message_identity_required() : fss_message(message_type_identity_required)
{
}

flight_safety_system::transport::fss_message_identity_required::fss_message_identity_required(uint64_t t_id, const std::shared_ptr<buf_len> &bl __attribute__((unused))) : fss_message(t_id, message_type_identity)
{
}


auto
flight_safety_system::transport::fss_message::decode(const std::shared_ptr<buf_len> &bl) -> std::shared_ptr<flight_safety_system::transport::fss_message>
{
    std::shared_ptr<fss_message> msg = nullptr;
    const char *data = bl->getData();
    auto type = (fss_message_type) ntohs(*(uint16_t *)(data + sizeof(uint16_t)));
    uint64_t msg_id = ntohll (*(uint64_t *)(data + sizeof(uint16_t) + sizeof(uint16_t)));

    switch (type)
    {
        case message_type_unknown:
        case message_type_closed:
            break;
        case message_type_identity:
            msg = std::make_shared<fss_message_identity>(msg_id, bl);
            break;
        case message_type_rtt_request:
            msg = std::make_shared<fss_message_rtt_request>(msg_id, bl);
            break;
        case message_type_rtt_response:
            msg = std::make_shared<fss_message_rtt_response>(msg_id, bl);
            break;
        case message_type_position_report:
            msg = std::make_shared<fss_message_position_report>(msg_id, bl);
            break;
        case message_type_system_status:
            msg = std::make_shared<fss_message_system_status>(msg_id, bl);
            break;
        case message_type_search_status:
            msg = std::make_shared<fss_message_search_status>(msg_id, bl);
            break;
        case message_type_command:
            msg = std::make_shared<fss_message_asset_command>(msg_id, bl);
            break;
        case message_type_server_list:
            msg = std::make_shared<fss_message_server_list>(msg_id, bl);
            break;
        case message_type_smm_settings:
            msg = std::make_shared<fss_message_smm_settings>(msg_id, bl);
            break;
        case message_type_identity_non_aircraft:
            msg = std::make_shared<fss_message_identity_non_aircraft>(msg_id, bl);
            break;
        case message_type_identity_required:
            msg = std::make_shared<fss_message_identity_required>(msg_id, bl);
            break;
    }
    
    return msg;
}
