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

TEST_CASE("rate_limiter: refill rate above 1000/s does not stall the clock")
{
    // For 1000 < refill_per_s < 2000 a single elapsed millisecond grants one
    // token but less than a full millisecond's worth, so converting the grant
    // back to whole milliseconds truncated to zero and stuck the refill clock,
    // letting tokens accrue at the call rate instead of the configured rate.
    fss::rate_limiter rl(2, 1500);
    REQUIRE(rl.consume(0));
    REQUIRE(rl.consume(0));
    REQUIRE(!rl.consume(0)); // bucket drained
    // 1ms later: 1.5 tokens accrue -> 1 whole token, 0.5 carried.
    REQUIRE(rl.consume(1));
    REQUIRE(!rl.consume(1)); // only one token was available, clock advanced
    // 1ms more: 0.5 carried + 1.5 = 2 tokens (capped at capacity 2).
    REQUIRE(rl.consume(2));
    REQUIRE(rl.consume(2));
    REQUIRE(!rl.consume(2));
}

TEST_CASE("rate_limiter: huge elapsed at a high refill rate does not overflow")
{
    // elapsed * refill_per_s is computed in uint64_t. With refill_per_s = 2^32
    // and an elapsed of 2^32 ms the product is exactly 2^64, which wraps to 0
    // -- so without clamping elapsed the drained bucket would never refill.
    // The clamp caps elapsed at a full bucket's worth, keeping the product
    // bounded and the refill correct.
    const uint64_t two_pow_32 = 4294967296ULL;
    fss::rate_limiter rl(3, two_pow_32);
    REQUIRE(rl.consume(0));
    REQUIRE(rl.consume(0));
    REQUIRE(rl.consume(0));
    REQUIRE(!rl.consume(0)); // bucket drained
    // The clamped elapsed refills to capacity; an unclamped multiply would
    // wrap to zero new tokens and leave the bucket wrongly empty.
    REQUIRE(rl.consume(two_pow_32));
    REQUIRE(rl.consume(two_pow_32));
    REQUIRE(rl.consume(two_pow_32));
    REQUIRE(!rl.consume(two_pow_32));
}
