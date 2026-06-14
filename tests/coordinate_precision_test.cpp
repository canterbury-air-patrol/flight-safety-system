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

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include "fss-transport.hpp"

using flight_safety_system::transport::fss_message;
using flight_safety_system::transport::fss_message_asset_command;
using flight_safety_system::transport::fss_message_position_report;
using flight_safety_system::transport::fss_message_system_status;
using flight_safety_system::transport::asset_command_hold;
using flight_safety_system::transport::asset_command_rtl;
using flight_safety_system::transport::FSS_COORD_SCALE;
using flight_safety_system::transport::FSS_VOLTAGE_SCALE;

/* Regression for todo/11-coordinate-constant.md
 *
 * Coordinates are transported as int32_t fixed-point with scale factor
 * FSS_COORD_SCALE (0.0000001 / 1e-7, defined in src/fss-transport.hpp).
 * This scale controls the precision of lat/long on the wire. The tests
 * below round-trip representative coordinates through pack+decode and
 * assert accuracy within the documented tolerance. A regression that
 * changes the scale, narrows the integer width, or introduces additional
 * precision loss will surface here.
 */

namespace {

auto round_trip(double lat, double lng) -> std::pair<double, double>
{
    auto original = std::make_shared<fss_message_position_report>(lat, lng, /*altitude*/ 100U,
                                                                  /*heading*/ 0U, /*hor_vel*/ 0U, /*ver_vel*/ 0,
                                                                  /*icao*/ 0U, std::string{"T1"},
                                                                  /*squawk*/ 01200U, /*tslc*/ 0U, /*flags*/ 0U,
                                                                  /*alt_type*/ 0U, /*emitter*/ 0U, /*timestamp*/ 0U);
    original->setId(1);
    auto bl = original->getPacked();
    auto decoded = std::dynamic_pointer_cast<fss_message_position_report>(fss_message::decode(bl));
    REQUIRE(decoded != nullptr);
    return {decoded->getLatitude(), decoded->getLongitude()};
}

auto round_trip_voltage(double voltage) -> double
{
    auto original = std::make_shared<fss_message_system_status>(uint8_t{80}, uint32_t{1000}, voltage);
    original->setId(1);
    auto decoded = std::dynamic_pointer_cast<fss_message_system_status>(fss_message::decode(original->getPacked()));
    REQUIRE(decoded != nullptr);
    return decoded->getBatVoltage();
}

} // namespace

TEST_CASE("coord: -43.5, 172.5 round-trip accurate to 1e-6")
{
    double lat = 0.0, lng = 0.0;
    std::tie(lat, lng) = round_trip(-43.5, 172.5);
    REQUIRE(std::fabs(lat - (-43.5)) < 1e-6);
    REQUIRE(std::fabs(lng - 172.5) < 1e-6);
}

TEST_CASE("coord: near +180 wraparound round-trip")
{
    double lat = 0.0, lng = 0.0;
    std::tie(lat, lng) = round_trip(0.0, 179.999999);
    REQUIRE(std::fabs(lng - 179.999999) < 1e-6);
}

TEST_CASE("coord: near -180 wraparound round-trip")
{
    double lat = 0.0, lng = 0.0;
    std::tie(lat, lng) = round_trip(0.0, -179.999999);
    REQUIRE(std::fabs(lng - (-179.999999)) < 1e-6);
}

TEST_CASE("coord: near zero round-trip")
{
    double lat = 0.0, lng = 0.0;
    std::tie(lat, lng) = round_trip(0.000001, 0.000001);
    REQUIRE(std::fabs(lat - 0.000001) < 1e-6);
    REQUIRE(std::fabs(lng - 0.000001) < 1e-6);
}

TEST_CASE("coord: exact zero round-trip")
{
    double lat = 0.0, lng = 0.0;
    std::tie(lat, lng) = round_trip(0.0, 0.0);
    REQUIRE(lat == 0.0);
    REQUIRE(lng == 0.0);
}

TEST_CASE("coord: southernmost latitudes round-trip")
{
    for (double target : {-85.0, -89.0, -89.999999})
    {
        double lat = 0.0, lng = 0.0;
        std::tie(lat, lng) = round_trip(target, 0.0);
        INFO("target latitude " << target);
        REQUIRE(std::fabs(lat - target) < 1e-6);
    }
}

TEST_CASE("coord: northernmost latitudes round-trip")
{
    for (double target : {85.0, 89.0, 89.999999})
    {
        double lat = 0.0, lng = 0.0;
        std::tie(lat, lng) = round_trip(target, 0.0);
        INFO("target latitude " << target);
        REQUIRE(std::fabs(lat - target) < 1e-6);
    }
}

TEST_CASE("coord: 7-decimal-place precision preserved")
{
    /* FSS_COORD_SCALE = 1e-7 gives int32 resolution of 0.1 µdegree, so
     * 7-decimal-place coordinates round-trip without loss. */
    double lat = 0.0, lng = 0.0;
    std::tie(lat, lng) = round_trip(-43.5000001, 172.5000001);
    REQUIRE(std::fabs(lat - (-43.5000001)) < 1e-7);
    REQUIRE(std::fabs(lng - 172.5000001) < 1e-7);
}

/* Regression for pack_scaled_coord NaN/Inf/out-of-range guard.
 *
 * Originally, passing NaN or Inf lat/long to pack_scaled_coord triggered
 * undefined behaviour in the double->int32_t cast; the first fix mapped them to
 * 0 (decoding to 0.0). As of the no-fix-sentinel change, non-finite input maps
 * to INT32_MIN on the wire and decodes back to NaN — "no GPS fix" must stay
 * distinct from (0,0), a legal coordinate (Null Island). Finite out-of-range
 * values are clamped into [INT32_MIN + 1, INT32_MAX] (the low side stops one
 * above the sentinel) so a clamped real value can never alias it.
 */

TEST_CASE("coord: asset_command RTL (NaN lat/lng) round-trips to NaN, not Null Island")
{
    /* RTL constructed with (command, timestamp) leaves latitude and longitude
     * as NaN (it carries no position). That must decode back to NaN, not to
     * (0,0) which would look like a real coordinate. */
    auto orig = std::make_shared<fss_message_asset_command>(asset_command_rtl, /*timestamp*/ 0ULL);
    orig->setId(1);
    auto bl = orig->getPacked();
    auto decoded = std::dynamic_pointer_cast<fss_message_asset_command>(fss_message::decode(bl));
    REQUIRE(decoded != nullptr);
    REQUIRE(std::isnan(decoded->getLatitude()));
    REQUIRE(std::isnan(decoded->getLongitude()));
}

TEST_CASE("coord: asset_command HOLD (NaN lat/lng) round-trips to NaN, not Null Island")
{
    auto orig = std::make_shared<fss_message_asset_command>(asset_command_hold, /*timestamp*/ 12345ULL);
    orig->setId(2);
    auto bl = orig->getPacked();
    auto decoded = std::dynamic_pointer_cast<fss_message_asset_command>(fss_message::decode(bl));
    REQUIRE(decoded != nullptr);
    REQUIRE(std::isnan(decoded->getLatitude()));
    REQUIRE(std::isnan(decoded->getLongitude()));
}

TEST_CASE("coord: position_report NaN lat/lng round-trips to NaN, not Null Island")
{
    /* A client with no GPS fix sends the default-NaN position. It must not be
     * indistinguishable from a real report at 0N 0E. */
    auto orig = std::make_shared<fss_message_position_report>(
        std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN(),
        /*altitude*/ 0U, /*heading*/ 0U, /*hor_vel*/ 0U, /*ver_vel*/ 0,
        /*icao*/ 0U, std::string{"T0"},
        /*squawk*/ 0U, /*tslc*/ 0U, /*flags*/ 0U,
        /*alt_type*/ 0U, /*emitter*/ 0U, /*timestamp*/ 0ULL);
    orig->setId(3);
    auto bl = orig->getPacked();
    auto decoded = std::dynamic_pointer_cast<fss_message_position_report>(fss_message::decode(bl));
    REQUIRE(decoded != nullptr);
    REQUIRE(std::isnan(decoded->getLatitude()));
    REQUIRE(std::isnan(decoded->getLongitude()));
}

TEST_CASE("coord: position_report Inf lat/lng round-trips to NaN, not Null Island")
{
    /* Infinity is also non-finite and maps to the no-fix sentinel. */
    auto orig = std::make_shared<fss_message_position_report>(
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
        /*altitude*/ 0U, /*heading*/ 0U, /*hor_vel*/ 0U, /*ver_vel*/ 0,
        /*icao*/ 0U, std::string{"T0"},
        /*squawk*/ 0U, /*tslc*/ 0U, /*flags*/ 0U,
        /*alt_type*/ 0U, /*emitter*/ 0U, /*timestamp*/ 0ULL);
    orig->setId(4);
    auto bl = orig->getPacked();
    auto decoded = std::dynamic_pointer_cast<fss_message_position_report>(fss_message::decode(bl));
    REQUIRE(decoded != nullptr);
    REQUIRE(std::isnan(decoded->getLatitude()));
    REQUIRE(std::isnan(decoded->getLongitude()));
}

TEST_CASE("coord: out-of-range latitude clamps rather than wrapping")
{
    /* A coordinate of 1e12 degrees scaled by 1e-7 exceeds INT32_MAX.
     * pack_scaled_coord must clamp to INT32_MAX rather than invoking UB. */
    double large = 1e12;
    double lat = 0.0, lng = 0.0;
    std::tie(lat, lng) = round_trip(large, 0.0);
    /* Must be finite and bounded by the valid coordinate space. */
    REQUIRE(std::isfinite(lat));
    /* Clamped to INT32_MAX * FSS_COORD_SCALE rather than wrapping. */
    REQUIRE(lat <= static_cast<double>(std::numeric_limits<int32_t>::max()) * FSS_COORD_SCALE);
    REQUIRE(lat > 0.0);
}

TEST_CASE("coord: large negative out-of-range latitude clamps rather than wrapping")
{
    double neg_large = -1e12;
    double lat = 0.0, lng = 0.0;
    std::tie(lat, lng) = round_trip(neg_large, 0.0);
    /* Clamps to INT32_MIN + 1, one above the no-fix sentinel, so the result
     * stays finite (isfinite) and must NOT alias the sentinel and decode to
     * NaN. This is what keeps a clamped real value distinct from "no fix". */
    REQUIRE(std::isfinite(lat));
    REQUIRE(lat >= static_cast<double>(std::numeric_limits<int32_t>::min() + 1) * FSS_COORD_SCALE);
    REQUIRE(lat < 0.0);
}

/* The system-status battery voltage is packed through the same scaled-int
 * helper, so it needs the same NaN/Inf/out-of-range guarding. */

TEST_CASE("voltage: NaN and Inf battery voltage pack to zero")
{
    REQUIRE(round_trip_voltage(std::numeric_limits<double>::quiet_NaN()) == 0.0);
    REQUIRE(round_trip_voltage(std::numeric_limits<double>::infinity()) == 0.0);
    REQUIRE(round_trip_voltage(-std::numeric_limits<double>::infinity()) == 0.0);
}

TEST_CASE("voltage: negative battery voltage clamps to zero")
{
    REQUIRE(round_trip_voltage(-5.0) == 0.0);
}

TEST_CASE("voltage: out-of-range battery voltage clamps rather than wrapping")
{
    const double voltage = round_trip_voltage(1e12);
    REQUIRE(std::isfinite(voltage));
    REQUIRE(voltage > 0.0);
    /* Clamped to INT32_MAX * FSS_VOLTAGE_SCALE rather than wrapping. */
    REQUIRE(voltage <= static_cast<double>(std::numeric_limits<int32_t>::max()) * FSS_VOLTAGE_SCALE);
}
