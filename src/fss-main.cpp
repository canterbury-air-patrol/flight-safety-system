#include "fss.hpp"

#include <sys/time.h>

auto
flight_safety_system::fss_current_timestamp() -> uint64_t
{
    struct timeval tv = {};
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000 + (tv.tv_usec / 1000);
}
