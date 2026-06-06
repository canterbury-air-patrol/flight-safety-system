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

#include <memory>
#include <string>

#include "fss-transport.hpp"
#include "test_helpers.hpp"

using flight_safety_system::transport::buf_len;
using flight_safety_system::transport::fss_message;
using flight_safety_system::transport::fss_message_identity;
using flight_safety_system::transport::fss_message_identity_non_aircraft;
using flight_safety_system::transport::fss_message_identity_required;
using flight_safety_system::transport::fss_message_position_report;
using flight_safety_system::transport::fss_message_rtt_request;
using flight_safety_system::transport::fss_message_rtt_response;
using flight_safety_system::transport::fss_message_search_status;
using flight_safety_system::transport::fss_message_asset_command;
using flight_safety_system::transport::fss_message_server_list;
using flight_safety_system::transport::fss_message_smm_settings;
using flight_safety_system::transport::fss_message_system_status;
using flight_safety_system::transport::asset_command_rtl;
using flight_safety_system::transport::asset_command_unknown;
using flight_safety_system::transport::message_type_command;
using flight_safety_system::transport::message_type_identity;
using flight_safety_system::transport::message_type_identity_non_aircraft;
using flight_safety_system::transport::message_type_identity_required;
using flight_safety_system::transport::message_type_position_report;
using flight_safety_system::transport::message_type_rtt_request;
using flight_safety_system::transport::message_type_rtt_response;
using flight_safety_system::transport::message_type_search_status;
using flight_safety_system::transport::message_type_server_list;
using flight_safety_system::transport::message_type_smm_settings;
using flight_safety_system::transport::message_type_system_status;
using flight_safety_system::transport::message_type_unknown;

TEST_CASE("malformed: decode returns nullptr for buffer shorter than header")
{
    /* Header is 2 + 2 + 8 = 12 bytes. Anything shorter must not crash. */
    auto bl = std::make_shared<buf_len>("\x00", 1);
    REQUIRE(fss_message::decode(bl) == nullptr);

    std::string eleven(11, '\0');
    auto bl2 = std::make_shared<buf_len>(eleven.data(), static_cast<uint16_t>(eleven.size()));
    REQUIRE(fss_message::decode(bl2) == nullptr);
}

TEST_CASE("malformed: decode returns nullptr for empty buffer")
{
    auto bl = std::make_shared<buf_len>();
    REQUIRE(fss_message::decode(bl) == nullptr);
}

TEST_CASE("malformed: decode returns nullptr for unknown message type")
{
    /* 0x00FF is an unused enum value; decode must not crash. */
    auto bl = fss_test::make_framed_buffer(0x00FFU, 42, "", 12);
    REQUIRE(fss_message::decode(bl) == nullptr);
}

TEST_CASE("malformed: decode returns nullptr for message_type_unknown")
{
    auto bl = fss_test::make_framed_buffer(static_cast<uint16_t>(message_type_unknown), 1, "", 12);
    REQUIRE(fss_message::decode(bl) == nullptr);
}

TEST_CASE("malformed: wrong-type dynamic_pointer_cast returns nullptr, no UB")
{
    /* Regression for todo/13-dynamic-cast-null-checks.md:
     * a message decoded as identity must not be cast-convertible to an
     * unrelated subclass; the cast should produce nullptr and the caller
     * must observe it without crashing. */
    auto original = std::make_shared<fss_message_identity>("name");
    original->setId(7);
    auto bl = original->getPacked();
    auto decoded = fss_message::decode(bl);
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_identity);

    auto wrong_cast = std::dynamic_pointer_cast<fss_message_position_report>(decoded);
    REQUIRE(wrong_cast == nullptr);
}

TEST_CASE("malformed: zero-length string field decodes to empty string")
{
    auto original = std::make_shared<fss_message_identity>("");
    original->setId(1);
    auto bl = original->getPacked();
    auto decoded = std::make_shared<fss_message_identity>(1, bl);
    REQUIRE(decoded->getName().empty());
}

/* Regression for todo/13: decode() must return a message whose getType()
 * matches the type field in the header.  These round-trip each concrete
 * message class through getPacked() → decode() and verify the invariant. */
TEST_CASE("decode type consistency: identity")
{
    auto orig = std::make_shared<fss_message_identity>("asset");
    orig->setId(1);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_identity);
    REQUIRE(std::dynamic_pointer_cast<fss_message_identity>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: rtt_request")
{
    auto orig = std::make_shared<fss_message_rtt_request>();
    orig->setId(2);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_rtt_request);
    REQUIRE(std::dynamic_pointer_cast<fss_message_rtt_request>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: rtt_response")
{
    auto orig = std::make_shared<fss_message_rtt_response>(99ULL);
    orig->setId(3);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_rtt_response);
    REQUIRE(std::dynamic_pointer_cast<fss_message_rtt_response>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: system_status")
{
    auto orig = std::make_shared<fss_message_system_status>(80, 1200, 12.4);
    orig->setId(4);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_system_status);
    REQUIRE(std::dynamic_pointer_cast<fss_message_system_status>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: search_status")
{
    auto orig = std::make_shared<fss_message_search_status>(1ULL, 50ULL, 100ULL);
    orig->setId(5);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_search_status);
    REQUIRE(std::dynamic_pointer_cast<fss_message_search_status>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: server_list")
{
    auto orig = std::make_shared<fss_message_server_list>();
    orig->setId(6);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_server_list);
    REQUIRE(std::dynamic_pointer_cast<fss_message_server_list>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: identity_non_aircraft")
{
    auto orig = std::make_shared<fss_message_identity_non_aircraft>();
    orig->setId(7);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_identity_non_aircraft);
    REQUIRE(std::dynamic_pointer_cast<fss_message_identity_non_aircraft>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: position_report")
{
    auto orig =
        std::make_shared<fss_message_position_report>(-33.8688, 151.2093, 100, 0, 0, 0, 0, "TEST", 0, 0, 0, 0, 0, 0ULL);
    orig->setId(11);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_position_report);
    REQUIRE(std::dynamic_pointer_cast<fss_message_position_report>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: command")
{
    auto orig = std::make_shared<fss_message_asset_command>(asset_command_rtl, 0ULL);
    orig->setId(9);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_command);
    REQUIRE(std::dynamic_pointer_cast<fss_message_asset_command>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: smm_settings")
{
    auto orig = std::make_shared<fss_message_smm_settings>(
        "http://example.com", flight_safety_system::secure_string(std::string_view("user")),
        flight_safety_system::secure_string(std::string_view("pass")));
    orig->setId(10);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_smm_settings);
    REQUIRE(std::dynamic_pointer_cast<fss_message_smm_settings>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: identity_required")
{
    auto orig = std::make_shared<fss_message_identity_required>();
    orig->setId(8);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_identity_required);
    REQUIRE(std::dynamic_pointer_cast<fss_message_identity_required>(decoded) != nullptr);
}

/* Regression for UB in fss_message_asset_command::unpackData: casting an
 * out-of-range wire byte to fss_asset_command was undefined behaviour.
 * After the fix decode_asset_command() validates the byte first and maps
 * any unrecognised value to asset_command_unknown. */
TEST_CASE("malformed: out-of-range asset command byte decodes to asset_command_unknown")
{
    /* Build a valid asset_command message via getPacked(), then replace the
     * last byte (the command byte on the wire) with an out-of-range value.
     * Copy the buffer into a mutable std::string first so the modification
     * is well-defined (getData() returns const char *). */
    auto orig = std::make_shared<fss_message_asset_command>(asset_command_rtl, 0ULL);
    orig->setId(1);
    auto packed = orig->getPacked();

    std::string wire(packed->getData(), packed->getLength());
    /* The command byte follows the framing header and the fixed asset_command
     * payload (timestamp, lat, lng, altitude). Derive its offset from the wire
     * field widths via sizeof so this stays correct if a field width changes;
     * the buffer is padded to a multiple of 8 bytes, so the trailing padding
     * must not be confused with the command byte. */
    static constexpr size_t header_len = 12U;
    static constexpr size_t cmd_offset =
        header_len + sizeof(uint64_t) + sizeof(int32_t) + sizeof(int32_t) + sizeof(uint32_t);
    REQUIRE(wire.size() > cmd_offset);
    /* 200 is not a defined fss_asset_command enumerator. */
    wire[cmd_offset] = static_cast<char>(200);

    auto bl = std::make_shared<buf_len>(wire.data(), static_cast<uint16_t>(wire.size()));
    auto decoded = std::dynamic_pointer_cast<fss_message_asset_command>(fss_message::decode(bl));
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getCommand() == asset_command_unknown);
}

/* Regression for todo/13: a cast to the wrong subclass must yield nullptr,
 * not a non-null pointer to a mismatched object — checked for every type. */
TEST_CASE("decode type consistency: wrong cast always returns nullptr")
{
    auto id_msg = std::make_shared<fss_message_identity>("x");
    id_msg->setId(1);
    auto decoded = fss_message::decode(id_msg->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(std::dynamic_pointer_cast<fss_message_rtt_response>(decoded) == nullptr);
    REQUIRE(std::dynamic_pointer_cast<fss_message_position_report>(decoded) == nullptr);
    REQUIRE(std::dynamic_pointer_cast<fss_message_system_status>(decoded) == nullptr);
    REQUIRE(std::dynamic_pointer_cast<fss_message_search_status>(decoded) == nullptr);
    REQUIRE(std::dynamic_pointer_cast<fss_message_server_list>(decoded) == nullptr);
}

/* Defensive: the length field in the header claims more bytes than the
 * buffer actually contains. decode() must not read past the buffer. */
TEST_CASE("malformed: oversized declared length does not read past buffer")
{
    /* Declare 0xFFFF in the length prefix but only provide a header + a tiny
     * payload. The identity decode path reads the declared length as the
     * string size and will over-read if the bug is not fixed. */
    std::string tiny_payload(4, 'A');
    auto bl = fss_test::make_framed_buffer(static_cast<uint16_t>(message_type_identity), 1, tiny_payload,
                                           /* declared_length */ 0xFFFFU);

    /* Correct behaviour: decode rejects the buffer (returns nullptr) or
     * returns a message whose name is bounded by the actual payload. */
    auto decoded = fss_message::decode(bl);
    if (decoded == nullptr)
    {
        SUCCEED("rejected oversized length");
    }
    else
    {
        auto ident = std::dynamic_pointer_cast<fss_message_identity>(decoded);
        REQUIRE(ident != nullptr);
        REQUIRE(ident->getName().size() <= tiny_payload.size());
    }
}
