#ifndef __FLIGHT_SAFETY_SYSTEM_HPP__
#define __FLIGHT_SAFETY_SYSTEM_HPP__

#include <string>
#include <cstring>
#include <queue>
#include <vector>
#include <cmath>
#include <thread>
#include <mutex>

#include <iostream>

#include <stdbool.h>

#include <sys/socket.h>
#include <netinet/in.h>

namespace flight_safety_system {

uint64_t fss_current_timestamp();
}

#endif
