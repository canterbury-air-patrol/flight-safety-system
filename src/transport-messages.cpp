#include "fss-transport.hpp"
#include "fss-endian.hpp"
#include "fss-log.hpp"

#include <cmath>
#include <limits>
#include <memory>

using flight_safety_system::fss_htobe16;
using flight_safety_system::fss_be16toh;
using flight_safety_system::fss_htobe32;
using flight_safety_system::fss_be32toh;
using flight_safety_system::fss_htobe64;
using flight_safety_system::fss_be64toh;
using flight_safety_system::transport::FSS_COORD_SCALE;
using flight_safety_system::transport::FSS_VOLTAGE_SCALE;

/* On-wire "no fix" sentinel for a scaled coordinate. INT32_MIN lies outside the
 * encodable coordinate range (±180° -> ±1.8e9, INT32_MIN ~= -2.147e9) so it can
 * never alias a real position; the receiver decodes it back to NaN (see
 * decode_coord), keeping "we don't know where the aircraft is" distinct from
 * (0,0) — Null Island, a legal coordinate. Finite values are clamped into
 * [coord_min_encodable, INT32_MAX], the low side stopping one above the
 * sentinel so a clamped real value can never collide with it. */
static constexpr int32_t coord_no_fix_sentinel = std::numeric_limits<int32_t>::min();
static constexpr int32_t coord_min_encodable = coord_no_fix_sentinel + 1;

/* Guard a double->int32_t conversion against NaN, Inf, and out-of-range values.
 * Non-finite input maps to the no-fix sentinel; finite values are clamped. */
static auto pack_scaled_coord(double value, double scale) -> int32_t
{
    if (!std::isfinite(value))
    {
        return coord_no_fix_sentinel;
    }
    const double scaled = value / scale;
    if (scaled <= static_cast<double>(coord_min_encodable))
    {
        return coord_min_encodable;
    }
    if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
    {
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(scaled);
}

/* Like pack_scaled_coord but clamps the low side at 0, for non-negative
 * quantities such as battery voltage so a stray negative never wraps. This also
 * deliberately collapses the no-fix sentinel (INT32_MIN, which is < 0) to 0:
 * voltage uses 0 as its own "unknown" convention and has no NaN sentinel. */
static auto pack_scaled_nonneg(double value, double scale) -> int32_t
{
    const int32_t packed = pack_scaled_coord(value, scale);
    return packed < 0 ? 0 : packed;
}

/* Validate an untrusted wire byte and map it to the matching fss_asset_command
 * enumerator.  Any value that is not a defined enumerator maps to
 * asset_command_unknown so that out-of-range bytes from a peer can never
 * produce undefined behaviour via an out-of-range enum cast. */
static auto decode_asset_command(uint8_t cmd) -> flight_safety_system::transport::fss_asset_command
{
    using namespace flight_safety_system::transport;
    switch (cmd)
    {
        case static_cast<uint8_t>(asset_command_rtl): return asset_command_rtl;
        case static_cast<uint8_t>(asset_command_hold): return asset_command_hold;
        case static_cast<uint8_t>(asset_command_goto): return asset_command_goto;
        case static_cast<uint8_t>(asset_command_resume): return asset_command_resume;
        case static_cast<uint8_t>(asset_command_terminate): return asset_command_terminate;
        case static_cast<uint8_t>(asset_command_disarm): return asset_command_disarm;
        case static_cast<uint8_t>(asset_command_altitude): return asset_command_altitude;
        case static_cast<uint8_t>(asset_command_manual): return asset_command_manual;
        default: return asset_command_unknown;
    }
}

static auto decode_command_ack_outcome(uint8_t outcome) -> flight_safety_system::transport::fss_command_ack_outcome
{
    using namespace flight_safety_system::transport;
    switch (outcome)
    {
        case static_cast<uint8_t>(command_ack_received): return command_ack_received;
        case static_cast<uint8_t>(command_ack_actioned): return command_ack_actioned;
        case static_cast<uint8_t>(command_ack_superseded): return command_ack_superseded;
        case static_cast<uint8_t>(command_ack_rejected): return command_ack_rejected;
        case static_cast<uint8_t>(command_ack_noop): return command_ack_noop;
        /* An unrecognised outcome from a newer peer degrades to "received": we
         * know the command reached the asset but cannot interpret the result. */
        default: return command_ack_received;
    }
}

static auto decode_command_ack_reason(uint8_t reason) -> flight_safety_system::transport::fss_command_ack_reason
{
    using namespace flight_safety_system::transport;
    switch (reason)
    {
        case static_cast<uint8_t>(supersede_none): return supersede_none;
        case static_cast<uint8_t>(supersede_low_battery): return supersede_low_battery;
        case static_cast<uint8_t>(supersede_comms_loss): return supersede_comms_loss;
        case static_cast<uint8_t>(supersede_newer_command): return supersede_newer_command;
        /* An unrecognised reason from a newer peer degrades to "none" rather
         * than inventing a cause the operator might act on. */
        default: return supersede_none;
    }
}

static void packStringRaw(const std::shared_ptr<flight_safety_system::transport::buf_len> &bl, const char *data,
                          size_t str_len)
{
    /* The on-wire length prefix is 16-bit. Clamp so the prefix can never
     * disagree with the bytes actually written; a mismatch would desync the
     * decoder. Legitimate strings are far below this (the receive path caps
     * whole messages at FSS_MAX_MESSAGE_BYTES), so clamping only guards
     * against an upstream programming error. */
    if (str_len > std::numeric_limits<uint16_t>::max())
    {
        FSS_LOG_ERROR("transport", "String of " << str_len << " bytes exceeds 16-bit length prefix; truncating");
        str_len = std::numeric_limits<uint16_t>::max();
    }
    uint16_t len = fss_htobe16(static_cast<uint16_t>(str_len));
    bl->addData(&len, sizeof(uint16_t));
    bl->addData(data, str_len);
    /* align to 8-byte boundary */
    uint64_t empty = 0;
    size_t pack_remainder = bl->getLength() % sizeof(uint64_t);
    if (pack_remainder != 0)
    {
        bl->addData(&empty, sizeof(uint64_t) - pack_remainder);
    }
}

void packString(const std::shared_ptr<flight_safety_system::transport::buf_len> &bl, const std::string &val)
{
    packStringRaw(bl, val.data(), val.size());
}

void packString(const std::shared_ptr<flight_safety_system::transport::buf_len> &bl,
                const flight_safety_system::secure_string &val)
{
    packStringRaw(bl, val.data(), val.size());
}

/* Advance a read offset to the next 8-byte boundary, clamped to the buffer
 * length. The clamp makes the postcondition offset <= length explicit: a
 * field ending near the buffer end can leave alignment padding that runs past
 * the end, and clamping ensures a later reader can never mistake that padding
 * for available bytes (which would wrap the size_t bounds check). */
static auto align_read_offset(size_t offset, size_t length) -> size_t
{
    size_t remainder = offset % sizeof(uint64_t);
    if (remainder != 0)
    {
        offset += sizeof(uint64_t) - remainder;
    }
    return offset > length ? length : offset;
}

template<typename StringType>
static auto unpackString(const char *data, size_t length, size_t &offset, StringType &result) -> bool
{
    if (offset > length || length - offset < sizeof(uint16_t))
    {
        return false;
    }
    uint16_t tmp;
    memcpy(&tmp, data + offset, sizeof(uint16_t));
    uint16_t len = fss_be16toh(tmp);
    offset += sizeof(uint16_t);
    if (len > length - offset)
    {
        return false;
    }
    result.assign(data + offset, len);
    offset += len;
    /* Align to the 8-byte boundary; align_read_offset clamps so the padding
     * past the buffer end can never be mistaken for available bytes. */
    offset = align_read_offset(offset, length);
    return true;
}

namespace {

class BufferReader {
    const char *m_data;
    size_t m_length;
    size_t m_offset;
    bool m_ok;
    auto ensureAvailable(size_t n) -> bool
    {
        /* m_offset > m_length is possible after unpackString aligns the
         * offset up past the end of the buffer; guard before subtracting so
         * the size_t arithmetic cannot wrap and admit an out-of-bounds read. */
        if (!m_ok || m_offset > m_length || m_length - m_offset < n)
        {
            m_ok = false;
            return false;
        }
        return true;
    }
public:
    BufferReader(const char *data, size_t length, size_t initial_offset)
        : m_data(data), m_length(length), m_offset(initial_offset), m_ok(initial_offset <= length)
    {
    }
    [[nodiscard]] auto ok() const -> bool { return m_ok; }
    auto readUint8(uint8_t &out) -> bool
    {
        if (!ensureAvailable(sizeof(uint8_t)))
            return false;
        out = static_cast<uint8_t>(m_data[m_offset]);
        m_offset += sizeof(uint8_t);
        return true;
    }
    auto readInt16(int16_t &out) -> bool
    {
        if (!ensureAvailable(sizeof(int16_t)))
            return false;
        int16_t tmp;
        memcpy(&tmp, m_data + m_offset, sizeof(int16_t));
        out = static_cast<int16_t>(fss_be16toh(static_cast<uint16_t>(tmp)));
        m_offset += sizeof(int16_t);
        return true;
    }
    auto readUint16(uint16_t &out) -> bool
    {
        if (!ensureAvailable(sizeof(uint16_t)))
            return false;
        uint16_t tmp;
        memcpy(&tmp, m_data + m_offset, sizeof(uint16_t));
        out = fss_be16toh(tmp);
        m_offset += sizeof(uint16_t);
        return true;
    }
    auto readInt32(int32_t &out) -> bool
    {
        if (!ensureAvailable(sizeof(int32_t)))
            return false;
        int32_t tmp;
        memcpy(&tmp, m_data + m_offset, sizeof(int32_t));
        out = static_cast<int32_t>(fss_be32toh(static_cast<uint32_t>(tmp)));
        m_offset += sizeof(int32_t);
        return true;
    }
    auto readUint32(uint32_t &out) -> bool
    {
        if (!ensureAvailable(sizeof(uint32_t)))
            return false;
        uint32_t tmp;
        memcpy(&tmp, m_data + m_offset, sizeof(uint32_t));
        out = fss_be32toh(tmp);
        m_offset += sizeof(uint32_t);
        return true;
    }
    auto readUint64(uint64_t &out) -> bool
    {
        if (!ensureAvailable(sizeof(uint64_t)))
            return false;
        uint64_t tmp;
        memcpy(&tmp, m_data + m_offset, sizeof(uint64_t));
        out = fss_be64toh(tmp);
        m_offset += sizeof(uint64_t);
        return true;
    }
    template<typename StringType> auto readString(StringType &out) -> bool
    {
        if (!m_ok || !unpackString(m_data, m_length, m_offset, out))
        {
            m_ok = false;
            return false;
        }
        return true;
    }
};

/* Decode one fixed-point coordinate, mapping the INT32_MIN "no fix" sentinel
 * (see pack_scaled_coord) back to NaN so it is never read as a real position. */
static auto decode_coord(int32_t raw) -> double
{
    if (raw == coord_no_fix_sentinel)
    {
        return NAN;
    }
    return static_cast<double>(raw) * FSS_COORD_SCALE;
}

/* Apply decoded fixed-point lat/lng to a message, but only if the buffer was
 * fully read: a truncated frame must decode to NaN, never to (0,0) — Null
 * Island is a legal coordinate that passes validation. The INT32_MIN no-fix
 * sentinel likewise decodes to NaN. Shared by the position-report and
 * asset-command decoders so the policy stays in one place. */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void assign_coordinates(const BufferReader &reader, int32_t lat, int32_t lng, double &out_lat, double &out_lng)
{
    if (reader.ok())
    {
        out_lat = decode_coord(lat);
        out_lng = decode_coord(lng);
    }
    else
    {
        out_lat = NAN;
        out_lng = NAN;
    }
}

} // namespace

flight_safety_system::transport::buf_len::buf_len(const buf_len &bl) = default;
flight_safety_system::transport::buf_len::buf_len(buf_len &&bl) noexcept = default;
flight_safety_system::transport::buf_len::buf_len() = default;

flight_safety_system::transport::buf_len::buf_len(const char *_data, uint16_t len) : data(_data, len) {}

flight_safety_system::transport::buf_len::buf_len(const void *_data, uint16_t len)
    : data(static_cast<const char *>(_data), len)
{
}

flight_safety_system::transport::buf_len::~buf_len() = default;

auto flight_safety_system::transport::buf_len::operator=(const buf_len &other) -> buf_len &
{
    if (this != &other)
    {
        this->data = other.data;
    }
    return *this;
}

auto flight_safety_system::transport::buf_len::isValid() -> bool
{
    return this->data.length() != 0;
}

auto flight_safety_system::transport::buf_len::addData(const char *new_data, size_t len) -> bool
{
    this->data.append(new_data, len);
    return true;
}

auto flight_safety_system::transport::buf_len::addData(const void *new_data, size_t len) -> bool
{
    this->data.append(static_cast<const char *>(new_data), len);
    return true;
}

void flight_safety_system::transport::buf_len::writeAt(size_t offset, const char *src, size_t len)
{
    this->data.replace(offset, len, src, len);
}

void flight_safety_system::transport::buf_len::writeAt(size_t offset, const void *src, size_t len)
{
    this->data.replace(offset, len, static_cast<const char *>(src), len);
}

auto flight_safety_system::transport::buf_len::getData() -> const char *
{
    return this->data.c_str();
}

auto flight_safety_system::transport::buf_len::getLength() -> size_t
{
    return this->data.length();
}

flight_safety_system::transport::fss_message_cb::fss_message_cb(std::shared_ptr<fss_connection> t_conn)
    : conn(std::move(t_conn))
{
}

flight_safety_system::transport::fss_message_cb::fss_message_cb(const fss_message_cb &from) : conn(nullptr)
{
    const std::scoped_lock lock(from.conn_lock);
    this->conn = from.conn;
}

void flight_safety_system::transport::fss_message_cb::setConnection(std::shared_ptr<fss_connection> t_conn)
{
    const std::scoped_lock lock(this->conn_lock);
    this->conn = std::move(t_conn);
}

void flight_safety_system::transport::fss_message_cb::clearConnection()
{
    const std::scoped_lock lock(this->conn_lock);
    this->conn = nullptr;
}

auto flight_safety_system::transport::fss_message_cb::operator=(
    const flight_safety_system::transport::fss_message_cb &other) -> fss_message_cb &
{
    if (this != &other)
    {
        const std::scoped_lock lock(this->conn_lock, other.conn_lock);
        this->conn = other.conn;
    }
    return *this;
}

auto flight_safety_system::transport::fss_message_cb::getConnection() -> std::shared_ptr<fss_connection>
{
    const std::scoped_lock lock(this->conn_lock);
    return this->conn;
}

auto flight_safety_system::transport::fss_message_cb::connected() -> bool
{
    const std::scoped_lock lock(this->conn_lock);
    return this->conn != nullptr;
}

void flight_safety_system::transport::fss_message_cb::disconnect()
{
    std::shared_ptr<fss_connection> c;
    {
        const std::scoped_lock lock(this->conn_lock);
        c = this->conn; // copy — conn must stay non-null while the recv thread runs
    }
    if (c)
    {
        c->disconnect(); // joins recv thread; processMessage may call getConnection() during this
        const std::scoped_lock lock(this->conn_lock);
        if (this->conn == c)
        {
            this->conn = nullptr;
        }
    }
}

auto flight_safety_system::transport::fss_message_cb::sendMsg(const std::shared_ptr<fss_message> &msg) -> bool
{
    std::shared_ptr<fss_connection> c;
    {
        const std::scoped_lock lock(this->conn_lock);
        c = this->conn;
    }
    return c ? c->sendMsg(msg) : false;
}

flight_safety_system::transport::fss_message::fss_message(fss_message_type t_type) : id(0), type(t_type) {}

flight_safety_system::transport::fss_message::fss_message(uint64_t t_id, fss_message_type t_type)
    : id(t_id), type(t_type)
{
}

flight_safety_system::transport::fss_message::~fss_message() = default;

auto flight_safety_system::transport::fss_message::headerLength() -> size_t
{
    return sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint64_t);
}

void flight_safety_system::transport::fss_message::setId(uint64_t t_id)
{
    this->id = t_id;
}
auto flight_safety_system::transport::fss_message::getId() -> uint64_t
{
    return this->id;
}
auto flight_safety_system::transport::fss_message::getSeq() -> uint64_t
{
    return this->id;
}
auto flight_safety_system::transport::fss_message::getType() -> fss_message_type
{
    return this->type;
}
auto flight_safety_system::transport::fss_message::getLatitude() -> double
{
    return NAN;
}
auto flight_safety_system::transport::fss_message::getLongitude() -> double
{
    return NAN;
}
auto flight_safety_system::transport::fss_message::getAltitude() -> uint32_t
{
    return 0;
}
auto flight_safety_system::transport::fss_message::getTimeStamp() -> uint64_t
{
    return 0;
}

void flight_safety_system::transport::fss_message::createHeader(const std::shared_ptr<buf_len> &bl)
{
    /* Make space for length (filled in by updateSize), type, id */
    uint16_t placeholder = 0;
    uint16_t type_n = fss_htobe16(this->getType());
    uint64_t id_n = fss_htobe64(this->getId());
    bl->addData(&placeholder, sizeof(uint16_t));
    bl->addData(&type_n, sizeof(uint16_t));
    bl->addData(&id_n, sizeof(uint64_t));
}

void flight_safety_system::transport::fss_message::updateSize(const std::shared_ptr<buf_len> &bl)
{
    size_t length = bl->getLength();
    if (length > sizeof(uint16_t))
    {
        /* The header length field is 16-bit. A message larger than that cannot
         * be framed; emitting a truncated length would corrupt the stream, so
         * log and leave the placeholder rather than writing a bogus value. The
         * receive side independently rejects anything over FSS_MAX_MESSAGE_BYTES. */
        if (length > std::numeric_limits<uint16_t>::max())
        {
            FSS_LOG_ERROR("transport", "Message of " << length << " bytes exceeds 16-bit length field; not framing");
            return;
        }
        /* Set the length (the unpadded content length; the receiver re-derives
         * the padded size). */
        if (length % sizeof(uint64_t) != 0)
        {
            uint64_t blank = 0;
            bl->addData(&blank, sizeof(uint64_t) - (length % sizeof(uint64_t)));
        }
        uint16_t length_n = fss_htobe16(static_cast<uint16_t>(length));
        bl->writeAt(0, &length_n, sizeof(uint16_t));
    }
}

auto flight_safety_system::transport::fss_message::getPacked() -> std::shared_ptr<buf_len>
{
    auto bl = std::make_shared<buf_len>();

    this->createHeader(bl);

    this->packData(bl);

    this->updateSize(bl);

    return bl;
}

void flight_safety_system::transport::fss_message_closed::packData(std::shared_ptr<buf_len> bl __attribute__((unused)))
{
}

flight_safety_system::transport::fss_message_closed::fss_message_closed() : fss_message(message_type_closed) {}

flight_safety_system::transport::fss_message_identity::fss_message_identity(std::string t_name)
    : fss_message(message_type_identity), name(std::move(t_name))
{
}

void flight_safety_system::transport::fss_message_identity::packData(std::shared_ptr<buf_len> bl)
{
    /* Deliberately NOT packString format: the name is raw bytes with no
     * length prefix, and the decoder derives its length from the message
     * header (see unpackData). Converting this to packString would break
     * wire compatibility with every deployed peer. */
    bl->addData(this->name.c_str(), this->name.length());
}

void flight_safety_system::transport::fss_message_identity::unpackData(const std::shared_ptr<buf_len> &bl)
{
    size_t offset = this->headerLength();
    const char *data = bl->getData();
    size_t length = bl->getLength();
    /* Use the recorded message length from the header if available,
       as bl may include alignment padding */
    if (length >= sizeof(uint16_t))
    {
        uint16_t msg_len;
        memcpy(&msg_len, data, sizeof(uint16_t));
        msg_len = fss_be16toh(msg_len);
        if (msg_len > 0 && msg_len <= length)
        {
            length = msg_len;
        }
    }
    if (length <= offset)
    {
        this->name.clear();
        return;
    }
    this->name.assign(data + offset, length - offset);
}

flight_safety_system::transport::fss_message_identity::fss_message_identity(uint64_t t_id,
                                                                            const std::shared_ptr<buf_len> &bl)
    : fss_message(t_id, message_type_identity), name()
{
    this->unpackData(bl);
}

auto flight_safety_system::transport::fss_message_identity::getName() -> std::string
{
    return this->name;
}


flight_safety_system::transport::fss_message_rtt_request::fss_message_rtt_request()
    : fss_message(message_type_rtt_request)
{
}

flight_safety_system::transport::fss_message_rtt_request::fss_message_rtt_request(uint64_t t_id,
                                                                                  const std::shared_ptr<buf_len> &bl
                                                                                  __attribute__((unused)))
    : fss_message(t_id, message_type_rtt_request)
{
}

void flight_safety_system::transport::fss_message_rtt_request::packData(std::shared_ptr<buf_len> bl
                                                                        __attribute__((unused)))
{
}

flight_safety_system::transport::fss_message_rtt_response::fss_message_rtt_response(uint64_t t_request_id)
    : fss_message(message_type_rtt_response), request_id(t_request_id)
{
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
flight_safety_system::transport::fss_message_rtt_response::fss_message_rtt_response(uint64_t t_request_id,
                                                                                    uint64_t t_client_timestamp)
    : fss_message(message_type_rtt_response), request_id(t_request_id), client_timestamp(t_client_timestamp)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
}

flight_safety_system::transport::fss_message_rtt_response::fss_message_rtt_response(uint64_t t_id,
                                                                                    const std::shared_ptr<buf_len> &bl)
    : fss_message(t_id, message_type_rtt_response), request_id(0)
{
    this->unpackData(bl);
}

void flight_safety_system::transport::fss_message_rtt_response::packData(std::shared_ptr<buf_len> bl)
{
    uint64_t data = fss_htobe64(this->request_id);
    bl->addData(&data, sizeof(uint64_t));
    /* The client timestamp is an optional trailing field: omitting it when 0
     * keeps the wire bytes identical to a legacy response, so a peer that never
     * reports its clock is unaffected. */
    if (this->client_timestamp != 0)
    {
        uint64_t ts = fss_htobe64(this->client_timestamp);
        bl->addData(&ts, sizeof(uint64_t));
    }
}

void flight_safety_system::transport::fss_message_rtt_response::unpackData(const std::shared_ptr<buf_len> &bl)
{
    BufferReader reader(bl->getData(), bl->getLength(), this->headerLength());
    if (!reader.readUint64(this->request_id))
    {
        this->request_id = 0;
    }
    /* Optional trailing field; left at 0 when absent (legacy response). */
    if (!reader.readUint64(this->client_timestamp))
    {
        this->client_timestamp = 0;
    }
}

auto flight_safety_system::transport::fss_message_rtt_response::getRequestId() -> uint64_t
{
    return this->request_id;
}

auto flight_safety_system::transport::fss_message_rtt_response::getClientTimestamp() -> uint64_t
{
    return this->client_timestamp;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
flight_safety_system::transport::fss_message_position_report::fss_message_position_report(
    double t_latitude, double t_longitude, uint32_t t_altitude, uint16_t t_heading, uint16_t t_hor_vel,
    int16_t t_ver_vel, uint32_t t_icao_address, std::string t_callsign, uint16_t t_squawk, uint8_t t_tslc,
    uint16_t t_flags, uint8_t t_alt_type, uint8_t t_emitter_type, uint64_t t_timestamp)
    : fss_message(message_type_position_report), latitude(t_latitude), longitude(t_longitude), altitude(t_altitude),
      heading(t_heading), horizontal_velocity(t_hor_vel), vertical_velocity(t_ver_vel), callsign(std::move(t_callsign)),
      icao_address(t_icao_address), squawk(t_squawk), timestamp(t_timestamp), tslc(t_tslc), flags(t_flags),
      altitude_type(t_alt_type), emitter_type(t_emitter_type)
{
}
// NOLINTEND(bugprone-easily-swappable-parameters)


flight_safety_system::transport::fss_message_position_report::fss_message_position_report(
    uint64_t t_id, const std::shared_ptr<buf_len> &bl)
    : fss_message(t_id, message_type_position_report)
{
    this->unpackData(bl);
}

void flight_safety_system::transport::fss_message_position_report::packData(std::shared_ptr<buf_len> bl)
{
    uint64_t ts = fss_htobe64(this->getTimeStamp());
    /* Convert the lat/long to fixed decimal for transport */
    const auto lat_host = pack_scaled_coord(this->getLatitude(), FSS_COORD_SCALE);
    const auto lng_host = pack_scaled_coord(this->getLongitude(), FSS_COORD_SCALE);
    const auto lat = static_cast<int32_t>(fss_htobe32(static_cast<uint32_t>(lat_host)));
    const auto lng = static_cast<int32_t>(fss_htobe32(static_cast<uint32_t>(lng_host)));
    uint32_t alt = fss_htobe32(this->getAltitude());
    uint32_t icao_id = fss_htobe32(this->getICAOAddress());
    uint16_t head = fss_htobe16(this->getHeading());
    uint16_t hor_vel = fss_htobe16(this->getHorzVel());
    const auto vv_host = static_cast<int16_t>(this->getVertVel());
    const auto ver_vel = static_cast<int16_t>(fss_htobe16(static_cast<uint16_t>(vv_host)));
    uint16_t squawk_code = fss_htobe16(this->getSquawk());
    uint16_t enc_flags = fss_htobe16(this->getFlags());

    bl->addData(&ts, sizeof(uint64_t));
    bl->addData(&lat, sizeof(int32_t));
    bl->addData(&lng, sizeof(int32_t));
    bl->addData(&alt, sizeof(uint32_t));
    bl->addData(&icao_id, sizeof(uint32_t));
    bl->addData(&head, sizeof(uint16_t));
    bl->addData(&hor_vel, sizeof(uint16_t));
    bl->addData(&ver_vel, sizeof(int16_t));
    bl->addData(&squawk_code, sizeof(uint16_t));
    packString(bl, this->getCallSign());
    bl->addData(&enc_flags, sizeof(uint16_t));
    bl->addData(&this->altitude_type, sizeof(uint8_t));
    bl->addData(&this->emitter_type, sizeof(uint8_t));
    bl->addData(&this->tslc, sizeof(uint8_t));
}

void flight_safety_system::transport::fss_message_position_report::unpackData(const std::shared_ptr<buf_len> &bl)
{
    BufferReader reader(bl->getData(), bl->getLength(), this->headerLength());
    this->timestamp = 0;
    this->altitude = 0;
    this->icao_address = 0;
    this->heading = 0;
    this->horizontal_velocity = 0;
    this->vertical_velocity = 0;
    this->squawk = 0;
    this->flags = 0;
    this->altitude_type = 0;
    this->emitter_type = 0;
    this->tslc = 0;
    int32_t lat = 0;
    int32_t lng = 0;
    reader.readUint64(this->timestamp);
    reader.readInt32(lat);
    reader.readInt32(lng);
    reader.readUint32(this->altitude);
    reader.readUint32(this->icao_address);
    reader.readUint16(this->heading);
    reader.readUint16(this->horizontal_velocity);
    reader.readInt16(this->vertical_velocity);
    reader.readUint16(this->squawk);
    reader.readString(this->callsign);
    reader.readUint16(this->flags);
    reader.readUint8(this->altitude_type);
    reader.readUint8(this->emitter_type);
    reader.readUint8(this->tslc);
    assign_coordinates(reader, lat, lng, this->latitude, this->longitude);
}

auto flight_safety_system::transport::fss_message_position_report::getLatitude() -> double
{
    return this->latitude;
}
auto flight_safety_system::transport::fss_message_position_report::getLongitude() -> double
{
    return this->longitude;
}
auto flight_safety_system::transport::fss_message_position_report::getAltitude() -> uint32_t
{
    return this->altitude;
}
auto flight_safety_system::transport::fss_message_position_report::getTimeStamp() -> uint64_t
{
    return this->timestamp;
}
auto flight_safety_system::transport::fss_message_position_report::getICAOAddress() -> uint32_t
{
    return this->icao_address;
}
auto flight_safety_system::transport::fss_message_position_report::getHeading() -> uint16_t
{
    return this->heading;
}
auto flight_safety_system::transport::fss_message_position_report::getHorzVel() -> uint16_t
{
    return this->horizontal_velocity;
}
auto flight_safety_system::transport::fss_message_position_report::getVertVel() -> int16_t
{
    return this->vertical_velocity;
}
auto flight_safety_system::transport::fss_message_position_report::getCallSign() -> std::string
{
    return this->callsign;
}
auto flight_safety_system::transport::fss_message_position_report::getSquawk() -> uint16_t
{
    return this->squawk;
}
auto flight_safety_system::transport::fss_message_position_report::getTSLC() -> uint8_t
{
    return this->tslc;
}
auto flight_safety_system::transport::fss_message_position_report::getFlags() -> uint16_t
{
    return this->flags;
}
auto flight_safety_system::transport::fss_message_position_report::getAltitudeType() -> uint8_t
{
    return this->altitude_type;
}
auto flight_safety_system::transport::fss_message_position_report::getEmitterType() -> uint8_t
{
    return this->emitter_type;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
flight_safety_system::transport::fss_message_system_status::fss_message_system_status(uint8_t bat_remaining_percent,
                                                                                      uint32_t bat_mah_used,
                                                                                      double bat_voltage)
    : fss_message(message_type_system_status), bat_percent(bat_remaining_percent), mah_used(bat_mah_used),
      voltage(bat_voltage)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
}

flight_safety_system::transport::fss_message_system_status::fss_message_system_status(
    uint64_t t_id, const std::shared_ptr<buf_len> &bl)
    : fss_message(t_id, message_type_system_status), bat_percent(0), mah_used(0), voltage(0.0)
{
    this->unpackData(bl);
}

void flight_safety_system::transport::fss_message_system_status::packData(std::shared_ptr<buf_len> bl)
{
    uint8_t bat_percent_n = this->getBatRemaining();
    uint32_t mah_used_n = fss_htobe32(this->getBatMAHUsed());
    uint32_t voltage_n =
        fss_htobe32(static_cast<uint32_t>(pack_scaled_nonneg(this->getBatVoltage(), FSS_VOLTAGE_SCALE)));

    bl->addData(&bat_percent_n, sizeof(uint8_t));
    bl->addData(&mah_used_n, sizeof(uint32_t));
    bl->addData(&voltage_n, sizeof(uint32_t));
}

void flight_safety_system::transport::fss_message_system_status::unpackData(const std::shared_ptr<buf_len> &bl)
{
    BufferReader reader(bl->getData(), bl->getLength(), this->headerLength());
    this->bat_percent = 0;
    this->mah_used = 0;
    this->voltage = 0.0;
    reader.readUint8(this->bat_percent);
    reader.readUint32(this->mah_used);
    uint32_t voltage_n = 0;
    if (reader.readUint32(voltage_n))
    {
        this->voltage = static_cast<double>(voltage_n) * FSS_VOLTAGE_SCALE;
    }
}

auto flight_safety_system::transport::fss_message_system_status::getBatRemaining() -> uint8_t
{
    return this->bat_percent;
}
auto flight_safety_system::transport::fss_message_system_status::getBatMAHUsed() -> uint32_t
{
    return this->mah_used;
}
auto flight_safety_system::transport::fss_message_system_status::getBatVoltage() -> double
{
    return this->voltage;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
flight_safety_system::transport::fss_message_search_status::fss_message_search_status(uint64_t t_search_id,
                                                                                      uint64_t last_point_completed,
                                                                                      uint64_t total_search_points)
    : fss_message(message_type_search_status), search_id(t_search_id), point_completed(last_point_completed),
      points_total(total_search_points)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
}

flight_safety_system::transport::fss_message_search_status::fss_message_search_status(
    uint64_t t_id, const std::shared_ptr<buf_len> &bl)
    : fss_message(t_id, message_type_search_status), search_id(0), point_completed(0), points_total(0)
{
    this->unpackData(bl);
}

void flight_safety_system::transport::fss_message_search_status::packData(std::shared_ptr<buf_len> bl)
{
    uint64_t search_id_n = fss_htobe64(this->getSearchId());
    uint64_t point_completed_n = fss_htobe64(this->getSearchCompleted());
    uint64_t points_total_n = fss_htobe64(this->getSearchTotal());
    bl->addData(&search_id_n, sizeof(uint64_t));
    bl->addData(&point_completed_n, sizeof(uint64_t));
    bl->addData(&points_total_n, sizeof(uint64_t));
}

void flight_safety_system::transport::fss_message_search_status::unpackData(const std::shared_ptr<buf_len> &bl)
{
    BufferReader reader(bl->getData(), bl->getLength(), this->headerLength());
    this->search_id = 0;
    this->point_completed = 0;
    this->points_total = 0;
    reader.readUint64(this->search_id);
    reader.readUint64(this->point_completed);
    reader.readUint64(this->points_total);
}

auto flight_safety_system::transport::fss_message_search_status::getSearchId() -> uint64_t
{
    return this->search_id;
}
auto flight_safety_system::transport::fss_message_search_status::getSearchCompleted() -> uint64_t
{
    return this->point_completed;
}
auto flight_safety_system::transport::fss_message_search_status::getSearchTotal() -> uint64_t
{
    return this->points_total;
}

flight_safety_system::transport::fss_message_asset_command::fss_message_asset_command(fss_asset_command t_command,
                                                                                      uint64_t t_timestamp)
    : fss_message(message_type_command), command(t_command), latitude(NAN), longitude(NAN), altitude(0),
      timestamp(t_timestamp)
{
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
flight_safety_system::transport::fss_message_asset_command::fss_message_asset_command(fss_asset_command t_command,
                                                                                      uint64_t t_timestamp,
                                                                                      double t_latitude,
                                                                                      double t_longitude)
    : fss_message(message_type_command), command(t_command), latitude(t_latitude), longitude(t_longitude), altitude(0),
      timestamp(t_timestamp)
{
}

flight_safety_system::transport::fss_message_asset_command::fss_message_asset_command(fss_asset_command t_command,
                                                                                      uint64_t t_timestamp,
                                                                                      uint32_t t_altitude)
    : fss_message(message_type_command), command(t_command), latitude(NAN), longitude(NAN), altitude(t_altitude),
      timestamp(t_timestamp)
{
}
// NOLINTEND(bugprone-easily-swappable-parameters)

flight_safety_system::transport::fss_message_asset_command::fss_message_asset_command(
    uint64_t t_id, const std::shared_ptr<buf_len> &bl)
    : fss_message(t_id, message_type_command), command(asset_command_unknown), latitude(NAN), longitude(NAN),
      altitude(0), timestamp(0)
{
    this->unpackData(bl);
}

void flight_safety_system::transport::fss_message_asset_command::packData(std::shared_ptr<buf_len> bl)
{
    uint64_t ts = fss_htobe64(this->getTimeStamp());
    /* Convert the lat/long to fixed decimal for transport */
    const auto lat_host = pack_scaled_coord(this->getLatitude(), FSS_COORD_SCALE);
    const auto lng_host = pack_scaled_coord(this->getLongitude(), FSS_COORD_SCALE);
    const auto lat = static_cast<int32_t>(fss_htobe32(static_cast<uint32_t>(lat_host)));
    const auto lng = static_cast<int32_t>(fss_htobe32(static_cast<uint32_t>(lng_host)));
    uint32_t alt = fss_htobe32(this->getAltitude());
    auto cmd = static_cast<uint8_t>(this->getCommand());
    bl->addData(&ts, sizeof(uint64_t));
    bl->addData(&lat, sizeof(int32_t));
    bl->addData(&lng, sizeof(int32_t));
    bl->addData(&alt, sizeof(uint32_t));
    bl->addData(&cmd, sizeof(uint8_t));
}

void flight_safety_system::transport::fss_message_asset_command::unpackData(const std::shared_ptr<buf_len> &bl)
{
    BufferReader reader(bl->getData(), bl->getLength(), this->headerLength());
    int32_t lat = 0;
    int32_t lng = 0;
    this->altitude = 0;
    this->timestamp = 0;
    reader.readUint64(this->timestamp);
    reader.readInt32(lat);
    reader.readInt32(lng);
    reader.readUint32(this->altitude);
    uint8_t cmd = 0;
    if (reader.readUint8(cmd))
    {
        this->command = decode_asset_command(cmd);
    }
    assign_coordinates(reader, lat, lng, this->latitude, this->longitude);
}

auto flight_safety_system::transport::fss_message_asset_command::getCommand() -> fss_asset_command
{
    return this->command;
}
auto flight_safety_system::transport::fss_message_asset_command::getLatitude() -> double
{
    return this->latitude;
}
auto flight_safety_system::transport::fss_message_asset_command::getLongitude() -> double
{
    return this->longitude;
}
auto flight_safety_system::transport::fss_message_asset_command::getAltitude() -> uint32_t
{
    return this->altitude;
}
auto flight_safety_system::transport::fss_message_asset_command::getTimeStamp() -> uint64_t
{
    return this->timestamp;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
flight_safety_system::transport::fss_message_command_ack::fss_message_command_ack(uint64_t t_acked_command_id,
                                                                                  fss_asset_command t_command,
                                                                                  fss_command_ack_outcome t_outcome,
                                                                                  uint64_t t_timestamp)
    : fss_message(message_type_command_ack), acked_command_id(t_acked_command_id),
      command(static_cast<uint8_t>(t_command)), outcome(static_cast<uint8_t>(t_outcome)),
      reason(static_cast<uint8_t>(supersede_none)), timestamp(t_timestamp)
{
}

flight_safety_system::transport::fss_message_command_ack::fss_message_command_ack(uint64_t t_acked_command_id,
                                                                                  fss_asset_command t_command,
                                                                                  fss_command_ack_outcome t_outcome,
                                                                                  fss_command_ack_reason t_reason,
                                                                                  uint64_t t_timestamp)
    : fss_message(message_type_command_ack), acked_command_id(t_acked_command_id),
      command(static_cast<uint8_t>(t_command)), outcome(static_cast<uint8_t>(t_outcome)),
      /* The reason is only meaningful for a superseded outcome; for any other
       * outcome it must read back as supersede_none. Enforce that here rather
       * than trusting every caller, so an inconsistent outcome/reason pair can
       * never propagate into the wire, logs, or UI. */
      reason(static_cast<uint8_t>(t_outcome == command_ack_superseded ? t_reason : supersede_none)),
      timestamp(t_timestamp)
{
}
// NOLINTEND(bugprone-easily-swappable-parameters)

flight_safety_system::transport::fss_message_command_ack::fss_message_command_ack(uint64_t t_id,
                                                                                  const std::shared_ptr<buf_len> &bl)
    : fss_message(t_id, message_type_command_ack)
{
    this->unpackData(bl);
}

void flight_safety_system::transport::fss_message_command_ack::packData(std::shared_ptr<buf_len> bl)
{
    uint64_t acked = fss_htobe64(this->acked_command_id);
    uint64_t ts = fss_htobe64(this->timestamp);
    bl->addData(&acked, sizeof(uint64_t));
    bl->addData(&this->command, sizeof(uint8_t));
    bl->addData(&this->outcome, sizeof(uint8_t));
    bl->addData(&this->reason, sizeof(uint8_t));
    bl->addData(&ts, sizeof(uint64_t));
}

void flight_safety_system::transport::fss_message_command_ack::unpackData(const std::shared_ptr<buf_len> &bl)
{
    BufferReader reader(bl->getData(), bl->getLength(), this->headerLength());
    this->acked_command_id = 0;
    this->command = static_cast<uint8_t>(asset_command_unknown);
    this->outcome = static_cast<uint8_t>(command_ack_received);
    this->reason = static_cast<uint8_t>(supersede_none);
    this->timestamp = 0;
    reader.readUint64(this->acked_command_id);
    reader.readUint8(this->command);
    reader.readUint8(this->outcome);
    reader.readUint8(this->reason);
    reader.readUint64(this->timestamp);
}

auto flight_safety_system::transport::fss_message_command_ack::getAckedCommandId() -> uint64_t
{
    return this->acked_command_id;
}

auto flight_safety_system::transport::fss_message_command_ack::getCommand() -> fss_asset_command
{
    return decode_asset_command(this->command);
}

auto flight_safety_system::transport::fss_message_command_ack::getOutcome() -> fss_command_ack_outcome
{
    return decode_command_ack_outcome(this->outcome);
}

auto flight_safety_system::transport::fss_message_command_ack::getReason() -> fss_command_ack_reason
{
    return decode_command_ack_reason(this->reason);
}

auto flight_safety_system::transport::fss_message_command_ack::getTimeStamp() -> uint64_t
{
    return this->timestamp;
}

void flight_safety_system::transport::fss_message_smm_settings::packData(std::shared_ptr<buf_len> bl)
{
    packString(bl, this->getServerURL());
    packString(bl, this->getUsername());
    packString(bl, this->getPassword());
}

void flight_safety_system::transport::fss_message_smm_settings::unpackData(const std::shared_ptr<buf_len> &bl)
{
    BufferReader reader(bl->getData(), bl->getLength(), this->headerLength());
    reader.readString(this->server_url);
    reader.readString(this->username);
    reader.readString(this->password);
}

flight_safety_system::transport::fss_message_smm_settings::fss_message_smm_settings(std::string t_server_url,
                                                                                    secure_string t_username,
                                                                                    secure_string t_password)
    : fss_message(message_type_smm_settings), server_url(std::move(t_server_url)), username(std::move(t_username)),
      password(std::move(t_password))
{
}

flight_safety_system::transport::fss_message_smm_settings::fss_message_smm_settings(uint64_t t_id,
                                                                                    const std::shared_ptr<buf_len> &bl)
    : fss_message(t_id, message_type_smm_settings), server_url(), username(), password()
{
    this->unpackData(bl);
}

auto flight_safety_system::transport::fss_message_smm_settings::getServerURL() -> std::string
{
    return this->server_url;
}

auto flight_safety_system::transport::fss_message_smm_settings::getUsername() -> const secure_string &
{
    return this->username;
}

auto flight_safety_system::transport::fss_message_smm_settings::getPassword() -> const secure_string &
{
    return this->password;
}

void packServer(const std::shared_ptr<flight_safety_system::transport::buf_len> &bl,
                const std::pair<std::string, uint16_t> &server)
{
    uint16_t port = fss_htobe16(server.second);
    bl->addData(&port, sizeof(port));
    packString(bl, server.first);
}

void flight_safety_system::transport::fss_message_server_list::packData(std::shared_ptr<buf_len> bl)
{
    for (const auto &server : this->servers)
    {
        packServer(bl, server);
    }
}

void flight_safety_system::transport::fss_message_server_list::unpackData(const std::shared_ptr<buf_len> &bl)
{
    BufferReader reader(bl->getData(), bl->getLength(), this->headerLength());
    while (reader.ok())
    {
        uint16_t port = 0;
        std::string server_addr;
        if (!reader.readUint16(port) || !reader.readString(server_addr))
        {
            break;
        }
        this->servers.emplace_back(server_addr, port);
    }
}

flight_safety_system::transport::fss_message_server_list::fss_message_server_list()
    : fss_message(message_type_server_list), servers()
{
}

flight_safety_system::transport::fss_message_server_list::fss_message_server_list(uint64_t t_id,
                                                                                  const std::shared_ptr<buf_len> &bl)
    : fss_message(t_id, message_type_server_list), servers()
{
    this->unpackData(bl);
}

void flight_safety_system::transport::fss_message_server_list::addServer(const std::string &server, uint16_t port)
{
    this->servers.emplace_back(server, port);
}

auto flight_safety_system::transport::fss_message_server_list::getServers()
    -> std::vector<std::pair<std::string, uint16_t>>
{
    return this->servers;
}


void flight_safety_system::transport::fss_message_identity_non_aircraft::packData(std::shared_ptr<buf_len> bl)
{
    uint64_t caps = fss_htobe64(this->capabilities);
    bl->addData(&caps, sizeof(uint64_t));
}

void flight_safety_system::transport::fss_message_identity_non_aircraft::unpackData(const std::shared_ptr<buf_len> &bl)
{
    BufferReader reader(bl->getData(), bl->getLength(), this->headerLength());
    this->capabilities = 0;
    reader.readUint64(this->capabilities);
}

flight_safety_system::transport::fss_message_identity_non_aircraft::fss_message_identity_non_aircraft()
    : fss_message(message_type_identity_non_aircraft)
{
}

flight_safety_system::transport::fss_message_identity_non_aircraft::fss_message_identity_non_aircraft(
    uint64_t t_id, const std::shared_ptr<buf_len> &bl)
    : fss_message(t_id, message_type_identity_non_aircraft)
{
    this->unpackData(bl);
}

void flight_safety_system::transport::fss_message_identity_non_aircraft::addCapability(uint8_t cap_id)
{
    /* Shifting by >= the type width is undefined behaviour; the bitmap holds
     * capability ids 0..63 only. An out-of-range id is a caller error, so
     * surface it (at debug, to avoid log noise) rather than silently no-op. */
    if (cap_id >= std::numeric_limits<uint64_t>::digits)
    {
        FSS_LOG_DEBUG("transport", "Ignoring out-of-range capability id " << static_cast<unsigned>(cap_id));
        return;
    }
    this->capabilities |= (static_cast<uint64_t>(1) << cap_id);
}

auto flight_safety_system::transport::fss_message_identity_non_aircraft::getCapability(uint8_t cap_id) -> bool
{
    if (cap_id >= std::numeric_limits<uint64_t>::digits)
    {
        FSS_LOG_DEBUG("transport", "Querying out-of-range capability id " << static_cast<unsigned>(cap_id));
        return false;
    }
    return !!(this->capabilities & (static_cast<uint64_t>(1) << cap_id));
}

void flight_safety_system::transport::fss_message_identity_required::packData(std::shared_ptr<buf_len> bl
                                                                              __attribute__((unused)))
{
}

flight_safety_system::transport::fss_message_identity_required::fss_message_identity_required()
    : fss_message(message_type_identity_required)
{
}

flight_safety_system::transport::fss_message_identity_required::fss_message_identity_required(
    uint64_t t_id, const std::shared_ptr<buf_len> &bl __attribute__((unused)))
    : fss_message(t_id, message_type_identity_required)
{
}

flight_safety_system::transport::fss_message_version::fss_message_version() : fss_message(message_type_version) {}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
flight_safety_system::transport::fss_message_version::fss_message_version(uint16_t t_version, uint16_t t_min_version,
                                                                          uint32_t t_flags)
    : fss_message(message_type_version), protocol_version(t_version), min_supported_version(t_min_version),
      feature_flags(t_flags)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
}

flight_safety_system::transport::fss_message_version::fss_message_version(uint64_t t_id,
                                                                          const std::shared_ptr<buf_len> &bl)
    : fss_message(t_id, message_type_version)
{
    this->unpackData(bl);
}

void flight_safety_system::transport::fss_message_version::packData(std::shared_ptr<buf_len> bl)
{
    uint16_t version_n = fss_htobe16(this->protocol_version);
    uint16_t min_version_n = fss_htobe16(this->min_supported_version);
    uint32_t flags_n = fss_htobe32(this->feature_flags);
    bl->addData(&version_n, sizeof(uint16_t));
    bl->addData(&min_version_n, sizeof(uint16_t));
    bl->addData(&flags_n, sizeof(uint32_t));
}

void flight_safety_system::transport::fss_message_version::unpackData(const std::shared_ptr<buf_len> &bl)
{
    BufferReader reader(bl->getData(), bl->getLength(), this->headerLength());
    this->protocol_version = 0;
    this->min_supported_version = 0;
    this->feature_flags = 0;
    reader.readUint16(this->protocol_version);
    reader.readUint16(this->min_supported_version);
    reader.readUint32(this->feature_flags);
}


auto flight_safety_system::transport::fss_message::decode(const std::shared_ptr<buf_len> &bl)
    -> std::shared_ptr<flight_safety_system::transport::fss_message>
{
    std::shared_ptr<fss_message> msg = nullptr;
    if (bl->getLength() < sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint64_t))
    {
        return msg;
    }
    const char *data = bl->getData();
    uint16_t type_n;
    memcpy(&type_n, data + sizeof(uint16_t), sizeof(uint16_t));
    auto type = static_cast<fss_message_type>(fss_be16toh(type_n));
    uint64_t msg_id;
    memcpy(&msg_id, data + sizeof(uint16_t) + sizeof(uint16_t), sizeof(uint64_t));
    msg_id = fss_be64toh(msg_id);

    switch (type)
    {
        case message_type_unknown:
        case message_type_closed: break;
        case message_type_identity: msg = std::make_shared<fss_message_identity>(msg_id, bl); break;
        case message_type_rtt_request: msg = std::make_shared<fss_message_rtt_request>(msg_id, bl); break;
        case message_type_rtt_response: msg = std::make_shared<fss_message_rtt_response>(msg_id, bl); break;
        case message_type_position_report: msg = std::make_shared<fss_message_position_report>(msg_id, bl); break;
        case message_type_system_status: msg = std::make_shared<fss_message_system_status>(msg_id, bl); break;
        case message_type_search_status: msg = std::make_shared<fss_message_search_status>(msg_id, bl); break;
        case message_type_command: msg = std::make_shared<fss_message_asset_command>(msg_id, bl); break;
        case message_type_server_list: msg = std::make_shared<fss_message_server_list>(msg_id, bl); break;
        case message_type_smm_settings: msg = std::make_shared<fss_message_smm_settings>(msg_id, bl); break;
        case message_type_identity_non_aircraft:
            msg = std::make_shared<fss_message_identity_non_aircraft>(msg_id, bl);
            break;
        case message_type_identity_required: msg = std::make_shared<fss_message_identity_required>(msg_id, bl); break;
        case message_type_version: msg = std::make_shared<fss_message_version>(msg_id, bl); break;
        case message_type_command_ack: msg = std::make_shared<fss_message_command_ack>(msg_id, bl); break;
    }

    if (msg != nullptr && msg->getType() != type)
    {
        return nullptr;
    }
    return msg;
}
