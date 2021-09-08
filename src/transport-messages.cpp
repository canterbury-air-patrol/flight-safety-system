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

void
flight_safety_system::transport::fss_message_cb::disconnect()
{
    this->conn->disconnect();
    this->conn = nullptr;
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

size_t
flight_safety_system::transport::fss_message::headerLength()
{
    return sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint64_t);
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

void
flight_safety_system::transport::fss_message_system_status::packData(std::shared_ptr<buf_len> bl)
{
    uint8_t bat_percent_n = this->getBatRemaining();
    uint32_t mah_used_n = htonl(this->getBatMAHUsed());
    bl->addData((char *)&bat_percent_n, sizeof(uint8_t));
    bl->addData((char *)&mah_used_n, sizeof(uint32_t));
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
    }
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

void
flight_safety_system::transport::fss_message_asset_command::packData(std::shared_ptr<buf_len> bl)
{
    uint64_t ts = htonll(this->getTimeStamp());
    /* Convert the lat/long to fixed decimal for transport */
    int32_t lat = htonl((int32_t) (this->getLatitude() / flt_to_int));
    int32_t lng = htonl((int32_t) (this->getLongitude() / flt_to_int));
    uint32_t alt = htonl(this->getAltitude());
    uint8_t cmd = (uint8_t) this->getCommand();
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

void
flight_safety_system::transport::fss_message_identity_non_aircraft::addCapability(uint8_t cap_id)
{
    this->capabilities |= ((uint64_t)(1) << cap_id);
}

bool
flight_safety_system::transport::fss_message_identity_non_aircraft::getCapability(uint8_t cap_id)
{
    return !!(this->capabilities & ((uint64_t)(1) << cap_id));
}

std::shared_ptr<flight_safety_system::transport::fss_message>
flight_safety_system::transport::fss_message::decode(const std::shared_ptr<buf_len> &bl)
{
    std::shared_ptr<fss_message> msg = nullptr;
    const char *data = bl->getData();
    fss_message_type type = (fss_message_type) ntohs(*(uint16_t *)(data + sizeof(uint16_t)));
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
    }
    
    return msg;
}
