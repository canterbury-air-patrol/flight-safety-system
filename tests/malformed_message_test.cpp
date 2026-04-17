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
using flight_safety_system::transport::fss_message_position_report;
using flight_safety_system::transport::message_type_identity;
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
    auto bl = fss_test::make_framed_buffer(
        static_cast<uint16_t>(message_type_unknown), 1, "", 12);
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

/* Defensive: the length field in the header claims more bytes than the
 * buffer actually contains. decode() must not read past the buffer. */
TEST_CASE("malformed: oversized declared length does not read past buffer")
{
    /* Declare 0xFFFF in the length prefix but only provide a header + a tiny
     * payload. The identity decode path reads the declared length as the
     * string size and will over-read if the bug is not fixed. */
    std::string tiny_payload(4, 'A');
    auto bl = fss_test::make_framed_buffer(
        static_cast<uint16_t>(message_type_identity),
        1,
        tiny_payload,
        /* declared_length */ 0xFFFFU);

    /* Correct behaviour: decode rejects the buffer (returns nullptr) or
     * returns a message whose name is bounded by the actual payload. */
    auto decoded = fss_message::decode(bl);
    if (decoded == nullptr) { SUCCEED("rejected oversized length"); }
    else
    {
        auto ident = std::dynamic_pointer_cast<fss_message_identity>(decoded);
        REQUIRE(ident != nullptr);
        REQUIRE(ident->getName().size() <= tiny_payload.size());
    }
}
