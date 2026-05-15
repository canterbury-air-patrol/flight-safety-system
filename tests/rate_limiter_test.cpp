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

#include "rate-limiter.hpp"

namespace fss = flight_safety_system;

TEST_CASE("rate_limiter: consumes tokens when bucket is full")
{
    fss::rate_limiter rl(10, 5);
    REQUIRE(rl.consume(0));
    REQUIRE(rl.consume(0));
}

TEST_CASE("rate_limiter: returns false when bucket is empty")
{
    fss::rate_limiter rl(2, 1);
    REQUIRE(rl.consume(0));
    REQUIRE(rl.consume(0));
    REQUIRE(!rl.consume(0));
}

TEST_CASE("rate_limiter: refills tokens after time passes")
{
    fss::rate_limiter rl(1, 10);
    REQUIRE(rl.consume(0));
    REQUIRE(!rl.consume(0));
    // refill_per_s=10 means 1 token per 100ms
    REQUIRE(rl.consume(100));
}

TEST_CASE("rate_limiter: does not refill beyond capacity")
{
    fss::rate_limiter rl(3, 10);
    rl.consume(0);
    rl.consume(0);
    rl.consume(0);
    REQUIRE(!rl.consume(0));
    // 1000ms at 10/s = 10 tokens refilled, but capped at capacity=3
    REQUIRE(rl.consume(1000));
    REQUIRE(rl.consume(1000));
    REQUIRE(rl.consume(1000));
    REQUIRE(!rl.consume(1000));
}

TEST_CASE("rate_limiter: zero refill rate never refills")
{
    fss::rate_limiter rl(2, 0);
    REQUIRE(rl.consume(0));
    REQUIRE(rl.consume(0));
    REQUIRE(!rl.consume(10000));
    REQUIRE(!rl.consume(10000));
}

TEST_CASE("rate_limiter: first consume initializes the refill clock")
{
    fss::rate_limiter rl(1, 10);
    // First consume at t=5000 should initialize last_refill_ms to 5000
    REQUIRE(rl.consume(5000));
    REQUIRE(!rl.consume(5000));
    // At t=5100 (100ms later, 1 token refilled)
    REQUIRE(rl.consume(5100));
}

TEST_CASE("rate_limiter: large elapsed time refills up to capacity")
{
    fss::rate_limiter rl(5, 1);
    // drain the bucket
    for (int i = 0; i < 5; i++)
    {
        rl.consume(0);
    }
    REQUIRE(!rl.consume(0));
    // 1 hour later: 3600 tokens would be added but capped at 5
    REQUIRE(rl.consume(3600000));
    REQUIRE(rl.consume(3600000));
    REQUIRE(rl.consume(3600000));
    REQUIRE(rl.consume(3600000));
    REQUIRE(rl.consume(3600000));
    REQUIRE(!rl.consume(3600000));
}

TEST_CASE("rate_limiter: cost > 1 consumes multiple tokens")
{
    fss::rate_limiter rl(5, 1);
    REQUIRE(rl.consume(0, 3));
    REQUIRE(rl.consume(0, 2));
    REQUIRE(!rl.consume(0, 1));
}

TEST_CASE("rate_limiter: cost greater than available tokens is rejected")
{
    fss::rate_limiter rl(3, 1);
    REQUIRE(!rl.consume(0, 4));
    // Bucket still has 3 tokens (none were consumed by the failed attempt)
    REQUIRE(rl.consume(0, 3));
}
