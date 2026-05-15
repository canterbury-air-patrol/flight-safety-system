#pragma once

#include <algorithm>
#include <cstdint>

namespace flight_safety_system {

class rate_limiter {
    uint64_t tokens;
    uint64_t max_tokens;
    uint64_t refill_per_s;
    uint64_t last_refill_ms{0};
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
            uint64_t elapsed = now_ms - last_refill_ms;
            uint64_t new_tokens = (refill_per_s > 0) ? (elapsed * refill_per_s / 1000) : 0;
            if (new_tokens > 0)
            {
                tokens = std::min(max_tokens, tokens + new_tokens);
                last_refill_ms += new_tokens * 1000 / refill_per_s;
            }
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
