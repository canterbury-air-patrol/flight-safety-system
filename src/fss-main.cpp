#include "fss.hpp"

#include <sys/time.h>

uint64_t
flight_safety_system::fss_current_timestamp()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000 + (tv.tv_usec / 1000);
}
