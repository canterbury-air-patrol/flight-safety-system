#pragma once

#include <string>
#include <cstring>
#include <queue>
#include <vector>
#include <cmath>
#include <thread>
#include <mutex>

#include <iostream>

#include <sys/socket.h>
#include <netinet/in.h>

namespace flight_safety_system {

auto fss_current_timestamp() -> uint64_t;
} // namespace flight_safety_system

