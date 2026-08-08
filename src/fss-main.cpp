#include "fss.hpp"

#include <ctime>
#include <sys/time.h>

auto flight_safety_system::fss_current_timestamp() -> uint64_t
{
    struct timeval tv = {};
    gettimeofday(&tv, nullptr);
    /* Widen before multiplying: on 32-bit time_t platforms (bookworm armhf)
     * tv_sec * 1000 overflows a 32-bit long and sign-extends into the return. */
    constexpr uint64_t sec_to_msec = 1000;
    constexpr uint64_t usec_to_msec = 1000;
    return static_cast<uint64_t>(tv.tv_sec) * sec_to_msec + static_cast<uint64_t>(tv.tv_usec) / usec_to_msec;
}

auto flight_safety_system::WallClock::now_ms() const -> uint64_t
{
    return flight_safety_system::fss_current_timestamp();
}

auto flight_safety_system::MonotonicClock::now_ms() const -> uint64_t
{
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    constexpr uint64_t sec_to_msec = 1000;
    constexpr uint64_t nsec_to_msec = 1000000;
    return static_cast<uint64_t>(ts.tv_sec) * sec_to_msec + static_cast<uint64_t>(ts.tv_nsec) / nsec_to_msec;
}
