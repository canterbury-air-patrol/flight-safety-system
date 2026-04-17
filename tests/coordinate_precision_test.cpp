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
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include "fss-transport.hpp"

using flight_safety_system::transport::fss_message;
using flight_safety_system::transport::fss_message_position_report;

/* Regression for todo/11-coordinate-constant.md
 *
 * Coordinates are transported as int32_t fixed-point with scale factor
 * `flt_to_int` (currently 0.000001f in src/transport-messages.cpp). This
 * scale controls the precision of lat/long on the wire. The tests below
 * round-trip representative coordinates through pack+decode and assert
 * accuracy within the documented tolerance. A regression that changes
 * the scale, narrows the integer width, or introduces additional
 * precision loss will surface here.
 *
 * The current code casts flt_to_int (float) up to double for division
 * and multiplication, so the *effective* scale is ~9.999999974752e-07
 * rather than exactly 1e-6. The tests target 1e-6 worst-case tolerance;
 * a tighter 1e-7 test is tagged [!shouldfail][bug11] and will start
 * passing once the todo lands and the scale becomes an exact double
 * (or the coord representation widens).
 */

namespace {

auto round_trip(double lat, double lng) -> std::pair<double, double>
{
    auto original = std::make_shared<fss_message_position_report>(
        lat, lng, /*altitude*/ 100U,
        /*heading*/ 0U, /*hor_vel*/ 0U, /*ver_vel*/ 0,
        /*icao*/ 0U, std::string{"T1"},
        /*squawk*/ 01200U, /*tslc*/ 0U, /*flags*/ 0U,
        /*alt_type*/ 0U, /*emitter*/ 0U, /*timestamp*/ 0U);
    original->setId(1);
    auto bl = original->getPacked();
    auto decoded = std::dynamic_pointer_cast<fss_message_position_report>(
        fss_message::decode(bl));
    REQUIRE(decoded != nullptr);
    return {decoded->getLatitude(), decoded->getLongitude()};
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

TEST_CASE("coord: 7-decimal-place precision preserved",
          "[!shouldfail][bug11]")
{
    /* With the current float scale constant, the effective multiplier
     * drifts from exact 1e-6 by ~2.5e-15 per unit count, so values that
     * should be representable to 7dp actually lose a digit. This test
     * documents the target precision once the scale becomes an exact
     * double (per todo/11). */
    double lat = 0.0, lng = 0.0;
    std::tie(lat, lng) = round_trip(-43.5000001, 172.5000001);
    REQUIRE(std::fabs(lat - (-43.5000001)) < 1e-7);
    REQUIRE(std::fabs(lng - 172.5000001) < 1e-7);
}
