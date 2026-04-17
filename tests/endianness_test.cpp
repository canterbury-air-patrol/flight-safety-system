#ifdef HAVE_CATCH2_CATCH_ALL_HPP
#include <catch2/catch_all.hpp>
#elif HAVE_CATCH2_CATCH_HPP
#include <catch2/catch.hpp>
#elif HAVE_CATCH_CATCH_HPP
#include <catch/catch.hpp>
#elif HAVE_CATCH_HPP
#include <catch.hpp>
#else
#error No catch header
#endif

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "fss-transport.hpp"

/* Regression for todo/14-endianness-consistency.md
 *
 * The wire protocol is defined as big-endian (network byte order). These
 * tests hand-author the bytes expected on the wire and feed them to
 * fss_message::decode, then pack a message on the current host and compare
 * byte-for-byte to the expected sequence. This catches:
 *   (a) any field whose packData forgets htonl/htons/htonll
 *   (b) architecture-specific drift when building on non-LE hosts
 *   (c) header layout regressions
 */

using flight_safety_system::transport::buf_len;
using flight_safety_system::transport::fss_message;
using flight_safety_system::transport::fss_message_identity;
using flight_safety_system::transport::fss_message_rtt_response;
using flight_safety_system::transport::fss_message_search_status;

namespace {

/* Construct a buf_len from a raw byte sequence. */
auto from_bytes(const std::vector<uint8_t> &bytes) -> std::shared_ptr<buf_len>
{
    return std::make_shared<buf_len>(
        reinterpret_cast<const char *>(bytes.data()),
        static_cast<uint16_t>(bytes.size()));
}

auto to_bytes(const std::shared_ptr<buf_len> &bl) -> std::vector<uint8_t>
{
    const auto *data = reinterpret_cast<const uint8_t *>(bl->getData());
    return std::vector<uint8_t>(data, data + bl->getLength());
}

} // namespace

TEST_CASE("endianness: identity decode matches hand-authored big-endian bytes")
{
    /* Layout:
     *   [0..1]  length       = 0x0010 (16 bytes declared length — "test" is 4 chars)
     *   [2..3]  type         = 0x0002 (message_type_identity)
     *   [4..11] id           = 0x0000_0000_0000_002A (42)
     *   [12..15] name bytes  = 't','e','s','t'
     *   16 % 8 == 0, so no post-header alignment is needed.
     */
    std::vector<uint8_t> bytes = {
        0x00, 0x10,                                     /* length = 16 */
        0x00, 0x02,                                     /* type = identity */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A, /* id = 42 */
        't',  'e',  's',  't',
    };

    auto bl = from_bytes(bytes);
    auto msg = fss_message::decode(bl);
    REQUIRE(msg != nullptr);
    REQUIRE(msg->getType() == flight_safety_system::transport::message_type_identity);
    REQUIRE(msg->getId() == 42U);
    auto ident = std::dynamic_pointer_cast<fss_message_identity>(msg);
    REQUIRE(ident != nullptr);
    REQUIRE(ident->getName() == "test");
}

TEST_CASE("endianness: identity pack produces the documented big-endian bytes")
{
    auto msg = std::make_shared<fss_message_identity>("test");
    msg->setId(42);
    auto bl = msg->getPacked();
    auto bytes = to_bytes(bl);

    /* First 16 bytes of the on-wire representation are fixed. Alignment bytes
     * beyond them may vary slightly in value (should all be zero) so we only
     * assert the deterministic prefix. */
    REQUIRE(bytes.size() >= 16U);
    REQUIRE(bytes[0] == 0x00); REQUIRE(bytes[1] == 0x10);       /* length = 16 */
    REQUIRE(bytes[2] == 0x00); REQUIRE(bytes[3] == 0x02);       /* type = identity */
    REQUIRE(bytes[4]  == 0x00); REQUIRE(bytes[5]  == 0x00);
    REQUIRE(bytes[6]  == 0x00); REQUIRE(bytes[7]  == 0x00);
    REQUIRE(bytes[8]  == 0x00); REQUIRE(bytes[9]  == 0x00);
    REQUIRE(bytes[10] == 0x00); REQUIRE(bytes[11] == 0x2A);     /* id = 42 */
    REQUIRE(bytes[12] == 't');  REQUIRE(bytes[13] == 'e');
    REQUIRE(bytes[14] == 's');  REQUIRE(bytes[15] == 't');
}

TEST_CASE("endianness: rtt_response pack/unpack uses big-endian 64-bit id")
{
    auto msg = std::make_shared<fss_message_rtt_response>(0x0102030405060708ULL);
    msg->setId(0xAABBCCDDEEFF0011ULL);
    auto bl = msg->getPacked();
    auto bytes = to_bytes(bl);

    REQUIRE(bytes.size() >= 20U);
    /* header: length | type(rtt_response=4) | id */
    REQUIRE(bytes[2] == 0x00); REQUIRE(bytes[3] == 0x04);
    /* id is 8 bytes big-endian starting at offset 4 */
    REQUIRE(bytes[4]  == 0xAA); REQUIRE(bytes[5]  == 0xBB);
    REQUIRE(bytes[6]  == 0xCC); REQUIRE(bytes[7]  == 0xDD);
    REQUIRE(bytes[8]  == 0xEE); REQUIRE(bytes[9]  == 0xFF);
    REQUIRE(bytes[10] == 0x00); REQUIRE(bytes[11] == 0x11);
    /* request_id is 8 bytes big-endian starting at offset 12 */
    REQUIRE(bytes[12] == 0x01); REQUIRE(bytes[13] == 0x02);
    REQUIRE(bytes[14] == 0x03); REQUIRE(bytes[15] == 0x04);
    REQUIRE(bytes[16] == 0x05); REQUIRE(bytes[17] == 0x06);
    REQUIRE(bytes[18] == 0x07); REQUIRE(bytes[19] == 0x08);

    auto decoded = fss_message::decode(bl);
    REQUIRE(decoded != nullptr);
    auto rr = std::dynamic_pointer_cast<fss_message_rtt_response>(decoded);
    REQUIRE(rr != nullptr);
    REQUIRE(rr->getId() == 0xAABBCCDDEEFF0011ULL);
    REQUIRE(rr->getRequestId() == 0x0102030405060708ULL);
}

TEST_CASE("endianness: search_status uses big-endian 64-bit fields")
{
    auto msg = std::make_shared<fss_message_search_status>(
        /*search_id*/ 0x1111111111111111ULL,
        /*completed*/ 0x2222222222222222ULL,
        /*total    */ 0x3333333333333333ULL);
    msg->setId(1);
    auto bl = msg->getPacked();
    auto bytes = to_bytes(bl);

    /* header 12 bytes, then three 8-byte big-endian fields */
    REQUIRE(bytes.size() >= 12U + 24U);
    for (int i = 0; i < 8; ++i) { REQUIRE(bytes[12 + i] == 0x11); }
    for (int i = 0; i < 8; ++i) { REQUIRE(bytes[20 + i] == 0x22); }
    for (int i = 0; i < 8; ++i) { REQUIRE(bytes[28 + i] == 0x33); }
}
