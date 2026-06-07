#pragma once

#include <string>
#include <cstring>
#include <queue>
#include <vector>
#include <cmath>
#include <thread>
#include <mutex>

#include <sys/socket.h>
#include <netinet/in.h>

namespace flight_safety_system {

auto fss_current_timestamp() -> uint64_t;

class IClock {
public:
    IClock() = default;
    IClock(const IClock &) = delete;
    IClock(IClock &&) = delete;
    auto operator=(const IClock &) -> IClock & = delete;
    auto operator=(IClock &&) -> IClock & = delete;
    virtual ~IClock() = default;
    virtual auto now_ms() const -> uint64_t = 0;
};

class WallClock : public IClock {
public:
    auto now_ms() const -> uint64_t override;
};

class MonotonicClock : public IClock {
public:
    auto now_ms() const -> uint64_t override;
};
} // namespace flight_safety_system
