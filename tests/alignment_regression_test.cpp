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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "fss-transport.hpp"

using flight_safety_system::transport::fss_message_position_report;

/* Regression for todo/10-string-alignment-bug.md
 *
 * packString() in src/transport-messages.cpp computes padding as
 *   pad = sizeof(uint64_t) - (bl->getLength() % sizeof(uint64_t));
 * which adds a full 8 bytes when the current buffer length is already
 * 8-byte aligned instead of adding 0. The correct calculation is
 *   pad = (sizeof(uint64_t) - (len % sizeof(uint64_t))) % sizeof(uint64_t);
 *
 * A position_report's callsign is the first field that flows through
 * packString, and by the time we reach it the buffer length is 44 bytes
 * (12-byte header + 32 bytes of fixed fields). A 2-char callsign brings
 * the pre-pad length to 48 (aligned), so the bug adds 8 spurious bytes.
 */

namespace {

/* Byte layout for position_report up to (and including) callsign, with fields
 * appended after. All fixed pieces; callsign alone varies. */
constexpr size_t header_bytes = 12;
constexpr size_t fixed_before_cs = 8 + 4 + 4 + 4 + 4 + 2 + 2 + 2 + 2; /* = 32 */
constexpr size_t fixed_after_cs = 2 + 1 + 1;                          /* = 4  */
constexpr size_t callsign_len_prefix = 2;
constexpr size_t align = 8;

auto pack_size_correct(size_t callsign_chars) -> size_t
{
    size_t pre_pad = header_bytes + fixed_before_cs + callsign_len_prefix + callsign_chars;
    size_t cs_pad = (align - (pre_pad % align)) % align;
    size_t after_cs = pre_pad + cs_pad + fixed_after_cs;
    size_t final_pad = (align - (after_cs % align)) % align;
    return after_cs + final_pad;
}

auto pack_position_report(const std::string &callsign) -> size_t
{
    constexpr double lat = -43.5;
    constexpr double lng = 172.5;
    auto msg = std::make_shared<fss_message_position_report>(lat, lng, /*altitude*/ 100U,
                                                             /*heading*/ 0U, /*hor_vel*/ 0U, /*ver_vel*/ 0,
                                                             /*icao*/ 0U, callsign,
                                                             /*squawk*/ 01200U, /*tslc*/ 0U, /*flags*/ 0U,
                                                             /*alt_type*/ 0U, /*emitter*/ 0U, /*timestamp*/ 0U);
    msg->setId(1);
    return msg->getPacked()->getLength();
}

} // namespace

TEST_CASE("alignment: non-aligned callsign lengths add no extra padding")
{
    /* These lengths never hit the aligned edge in the buggy formula, so they
     * must pass both before and after the fix — guarding against regression
     * the other way. */
    for (size_t L :
         {size_t{0}, size_t{1}, size_t{3}, size_t{4}, size_t{5}, size_t{7}, size_t{9}, size_t{15}, size_t{17}})
    {
        std::string cs(L, 'X');
        INFO("callsign length " << L);
        REQUIRE(pack_position_report(cs) == pack_size_correct(L));
    }
}

TEST_CASE("alignment: 8-byte-aligned callsign length adds 0 padding, not 8", "[bug10]")
{
    /* L where (header + fixed + 2 + L) % 8 == 0 triggers the bug.
     * header(12) + fixed_before_cs(32) + 2 = 46; L such that (46 + L) % 8 == 0:
     *   L ∈ {2, 10, 18, 26, ...}
     */
    for (size_t L : {size_t{2}, size_t{10}, size_t{18}})
    {
        std::string cs(L, 'X');
        INFO("callsign length " << L << " — aligned-edge case");
        REQUIRE(pack_position_report(cs) == pack_size_correct(L));
    }
}
