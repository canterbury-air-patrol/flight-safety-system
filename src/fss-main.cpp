#include "fss.hpp"

#include <sys/time.h>

auto
flight_safety_system::fss_current_timestamp() -> uint64_t
{
    struct timeval tv = {};
    gettimeofday(&tv, nullptr);
    constexpr int sec_to_msec = 1000;
    constexpr int usec_to_msec = 1000;
    return tv.tv_sec * sec_to_msec + (tv.tv_usec / usec_to_msec);
}

auto
flight_safety_system::WallClock::now_ms() const -> uint64_t
{
    return flight_safety_system::fss_current_timestamp();
}
