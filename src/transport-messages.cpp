#include "fss-transport.hpp"

#include <arpa/inet.h>
#if __APPLE__
/* Apple already has these defines */
#else
#include <endian.h>
#define ntohll(x) be64toh(x)
#define htonll(x) htobe64(x)
#endif

using namespace flight_safety_system::transport;

size_t
fss_message::headerLength()
{
    return sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint64_t);
}

void
fss_message::createHeader(buf_len *bl)
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
fss_message::updateSize(buf_len *bl)
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

buf_len *
fss_message::getPacked()
{
    auto bl = new buf_len();

    this->createHeader(bl);

    this->packData(bl);

    this->updateSize(bl);

    return bl;
}

void
fss_message_identity::packData(buf_len *bl)
{
    bl->addData(this->name.c_str(), this->name.length());
}

void
fss_message_identity::unpackData(buf_len *bl)
{
    size_t offset = this->headerLength();
    char *data = bl->getData();
    size_t length = bl->getLength();
    char *name_n = (char *)calloc(1, (length - offset) + 1);
    memcpy(name_n, data + offset, length - offset);
    name_n[length - offset] = '\0';
    this->name = std::string(name_n);
    free (name_n);
}

void
fss_message_rtt_response::packData(buf_len *bl)
{
    uint64_t data = htonll(this->request_id);
    bl->addData((char *)&data, sizeof(uint64_t));
}

void
fss_message_rtt_response::unpackData(buf_len *bl)
{
    size_t offset = this->headerLength();
    char *data = bl->getData();
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

void
fss_message_position_report::packData(buf_len *bl)
{
    uint64_t ts = htonll(this->getTimeStamp());
    /* Convert the lat/long to fixed decimal for transport */
    int32_t lat = htonl((int32_t) (this->getLatitude() / 0.000001));
    int32_t lng = htonl((int32_t) (this->getLongitude() / 0.000001));
    uint32_t alt = htonl(this->getAltitude());
    bl->addData((char *)&ts, sizeof(uint64_t));
    bl->addData((char *)&lat, sizeof(int32_t));
    bl->addData((char *)&lng, sizeof(int32_t));
    bl->addData((char *)&alt, sizeof(uint32_t));
}

void
fss_message_position_report::unpackData(buf_len *bl)
{
    size_t offset = this->headerLength();
    char *data = bl->getData();
    size_t length = bl->getLength();
    int32_t lat = 0;
    int32_t lng = 0;
    this->altitude = 0;
    this->timestamp = 0;
    if (length - offset == (sizeof(uint64_t) + sizeof(int32_t) + sizeof(int32_t) + sizeof(uint32_t)))
    {
        this->timestamp = ntohll(*(uint64_t *)(data + offset));
        offset += sizeof(uint64_t);
        lat = ntohl(*(int32_t *)(data + offset));
        offset += sizeof(int32_t);
        lng = ntohl(*(int32_t *)(data + offset));
        offset += sizeof(int32_t);
        this->altitude = ntohl(*(uint32_t *)(data + offset));
    }
    this->latitude = ((double)lat) * 0.000001;
    this->longitude = ((double)lng) * 0.000001;
}

void
fss_message_system_status::packData(buf_len *bl)
{
    uint8_t bat_percent_n = this->getBatRemaining();
    uint32_t mah_used_n = htonl(this->getBatMAHUsed());
    bl->addData((char *)&bat_percent_n, sizeof(uint8_t));
    bl->addData((char *)&mah_used_n, sizeof(uint32_t));
}

void
fss_message_system_status::unpackData(buf_len *bl)
{
    size_t offset = this->headerLength();
    char *data = bl->getData();
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
fss_message_search_status::packData(buf_len *bl)
{
    uint64_t search_id_n = htonll(this->getSearchId());
    uint64_t point_completed_n = htonll(this->getSearchCompleted());
    uint64_t points_total_n = htonll(this->getSearchTotal());
    bl->addData((char *)&search_id_n, sizeof(uint64_t));
    bl->addData((char *)&point_completed_n, sizeof(uint64_t));
    bl->addData((char *)&points_total_n, sizeof(uint64_t));
}

void
fss_message_search_status::unpackData(buf_len *bl)
{
    size_t offset = this->headerLength();
    char *data = bl->getData();
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
fss_message_asset_command::packData(buf_len *bl)
{
    uint64_t ts = htonll(this->getTimeStamp());
    /* Convert the lat/long to fixed decimal for transport */
    int32_t lat = htonl((int32_t) (this->getLatitude() / 0.000001));
    int32_t lng = htonl((int32_t) (this->getLongitude() / 0.000001));
    uint32_t alt = htonl(this->getAltitude());
    uint8_t cmd = (uint8_t) this->getCommand();
    bl->addData((char *)&ts, sizeof(uint64_t));
    bl->addData((char *)&lat, sizeof(int32_t));
    bl->addData((char *)&lng, sizeof(int32_t));
    bl->addData((char *)&alt, sizeof(uint32_t));
    bl->addData((char *)&cmd, sizeof(uint8_t));
}

void
fss_message_asset_command::unpackData(buf_len *bl)
{
    size_t offset = this->headerLength();
    char *data = bl->getData();
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
    this->latitude = lat * 0.000001;
    this->longitude = lng * 0.000001;
}

void
packString(buf_len *bl, std::string val)
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
fss_message_smm_settings::packData(buf_len *bl)
{
    packString(bl, this->getServerURL());
    packString(bl, this->getUsername());
    packString(bl, this->getPassword());
}

void
fss_message_smm_settings::unpackData(buf_len *bl)
{
    size_t offset = this->headerLength();
    char *data = bl->getData();
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
packServer(buf_len *bl, std::pair<std::string, uint16_t> server)
{
    uint16_t port = htons(server.second);
    bl->addData((char *)&port, sizeof(port));
    packString(bl, server.first);
}

void
fss_message_server_list::packData(buf_len *bl)
{
    for(auto server : this->servers)
    {
        packServer(bl, server);
    }
}

void
fss_message_server_list::unpackData(buf_len *bl)
{
    size_t offset = this->headerLength();
    char *data = bl->getData();
    size_t length = bl->getLength();
    while (length - offset >= (sizeof(uint16_t) + sizeof(uint16_t)))
    {
        uint16_t port = ntohs(*(uint16_t *)(data + offset));
        offset += sizeof(uint16_t);
        uint16_t len = ntohs(*(uint16_t *)(data + offset));
        offset += sizeof(uint16_t);
        std::string server_addr;
        server_addr.assign((char *)(data + offset), len);
        this->servers.push_back(std::make_pair(server_addr, port));
        offset += len;
        offset += (sizeof(uint64_t) - offset % sizeof(uint64_t));
    }
}

fss_message *
fss_message::decode(buf_len *bl)
{
    fss_message *msg = nullptr;
    char *data = bl->getData();
    fss_message_type type = (fss_message_type) ntohs(*(uint16_t *)(data + sizeof(uint16_t)));
    uint64_t msg_id = ntohll (*(uint64_t *)(data + sizeof(uint16_t) + sizeof(uint16_t)));

    switch (type)
    {
        case message_type_unknown:
        case message_type_closed:
            break;
        case message_type_identity:
            msg = new fss_message_identity(msg_id, bl);
            break;
        case message_type_rtt_request:
            msg = new fss_message_rtt_request(msg_id, bl);
            break;
        case message_type_rtt_response:
            msg = new fss_message_rtt_response(msg_id, bl);
            break;
        case message_type_position_report:
            msg = new fss_message_position_report(msg_id, bl);
            break;
        case message_type_system_status:
            msg = new fss_message_system_status(msg_id, bl);
            break;
        case message_type_search_status:
            msg = new fss_message_search_status(msg_id, bl);
            break;
        case message_type_command:
            msg = new fss_message_asset_command(msg_id, bl);
            break;
        case message_type_server_list:
            msg = new fss_message_server_list(msg_id, bl);
            break;
        case message_type_smm_settings:
            msg = new fss_message_smm_settings(msg_id, bl);
            break;
    }
    
    return msg;
}
