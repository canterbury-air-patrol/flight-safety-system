#pragma once

#include <algorithm>
#include <cstdint>

namespace flight_safety_system {

class rate_limiter {
    uint64_t tokens;
    uint64_t max_tokens;
    uint64_t refill_per_s;
    uint64_t last_refill_ms{0};
    // Sub-token carry: the numerator (elapsed_ms * refill_per_s) left over
    // after granting whole tokens, always < 1000. Carrying it keeps the
    // refill clock accurate for refill rates above 1000/s, where converting
    // the granted tokens back to whole milliseconds would truncate to zero
    // and stall the clock.
    uint64_t refill_carry{0};
    bool initialized{false};
public:
    rate_limiter(uint64_t capacity, uint64_t t_refill_per_s)
        : tokens(capacity), max_tokens(capacity), refill_per_s(t_refill_per_s)
    {
    }

    auto consume(uint64_t now_ms, uint64_t cost = 1) -> bool
    {
        if (!initialized)
        {
            initialized = true;
            last_refill_ms = now_ms;
        }
        if (now_ms > last_refill_ms)
        {
            if (refill_per_s > 0)
            {
                uint64_t elapsed = now_ms - last_refill_ms;
                // Once enough time has passed to refill a full bucket, any
                // extra only tops it off (tokens saturate at max_tokens). Cap
                // elapsed at that point so elapsed * refill_per_s below stays
                // bounded by ~max_tokens * 1000 and cannot overflow uint64_t
                // after a very long idle gap or at a high refill rate.
                uint64_t fill_ms = max_tokens * 1000 / refill_per_s + 1;
                if (elapsed > fill_ms)
                {
                    elapsed = fill_ms;
                    refill_carry = 0;
                }
                uint64_t numerator = elapsed * refill_per_s + refill_carry;
                uint64_t new_tokens = numerator / 1000;
                refill_carry = numerator % 1000;
                if (new_tokens > 0)
                {
                    tokens = std::min(max_tokens, tokens + new_tokens);
                }
            }
            last_refill_ms = now_ms;
        }
        if (tokens < cost)
        {
            return false;
        }
        tokens -= cost;
        return true;
    }
};

} // namespace flight_safety_system
