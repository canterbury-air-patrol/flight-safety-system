#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace flight_safety_system {
namespace log {

enum class level : int {
    ERROR = 0,
    WARN  = 1,
    INFO  = 2,
    DEBUG = 3,
};

namespace detail {

inline std::atomic<level> & current_level()
{
    static std::atomic<level> lvl{[]() -> level {
        level l = level::INFO;
        const char *env = std::getenv("FSS_LOG_LEVEL");
        if (env == nullptr) { return l; }
        std::string s{env};
        if (s == "error" || s == "ERROR")      { l = level::ERROR; }
        else if (s == "warn"  || s == "WARN")  { l = level::WARN;  }
        else if (s == "info"  || s == "INFO")  { l = level::INFO;  }
        else if (s == "debug" || s == "DEBUG") { l = level::DEBUG; }
        return l;
    }()};
    return lvl;
}

inline std::mutex & log_mutex()
{
    static std::mutex m;
    return m;
}

inline const char * level_str(level lvl)
{
    switch (lvl)
    {
        case level::ERROR: return "ERROR";
        case level::WARN:  return "WARN ";
        case level::INFO:  return "INFO ";
        case level::DEBUG: return "DEBUG";
    }
    return "?????";
}

} // namespace detail

inline void set_level(const std::string &lvl_str)
{
    if (lvl_str == "error" || lvl_str == "ERROR")      { detail::current_level() = level::ERROR; }
    else if (lvl_str == "warn"  || lvl_str == "WARN")  { detail::current_level() = level::WARN;  }
    else if (lvl_str == "info"  || lvl_str == "INFO")  { detail::current_level() = level::INFO;  }
    else if (lvl_str == "debug" || lvl_str == "DEBUG") { detail::current_level() = level::DEBUG; }
}

inline void write(level lvl, const char *component, const std::string &msg)
{
    std::lock_guard<std::mutex> guard(detail::log_mutex());
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[21]{};
    struct tm gm_tm{};
    gmtime_r(&t, &gm_tm);
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &gm_tm);
    std::cerr << buf
              << " [" << detail::level_str(lvl) << "]"
              << " [" << component << "] "
              << msg << "\n";
}

} // namespace log
} // namespace flight_safety_system

/* Stream-expression logging: FSS_LOG_ERROR("comp", "val=" << val) */
#define FSS_LOG(lvl, comp, ...) \
    do { \
        auto _fss_log_lvl = (lvl); \
        if (_fss_log_lvl <= flight_safety_system::log::detail::current_level()) { \
            std::ostringstream _fss_log_oss; \
            _fss_log_oss << __VA_ARGS__; \
            flight_safety_system::log::write(_fss_log_lvl, (comp), _fss_log_oss.str()); \
        } \
    } while (false)

#define FSS_LOG_ERROR(comp, ...) FSS_LOG(flight_safety_system::log::level::ERROR, (comp), __VA_ARGS__)
#define FSS_LOG_WARN(comp, ...)  FSS_LOG(flight_safety_system::log::level::WARN,  (comp), __VA_ARGS__)
#define FSS_LOG_INFO(comp, ...)  FSS_LOG(flight_safety_system::log::level::INFO,  (comp), __VA_ARGS__)
#define FSS_LOG_DEBUG(comp, ...) FSS_LOG(flight_safety_system::log::level::DEBUG, (comp), __VA_ARGS__)

/* perror() replacement: appends ": strerror(errno)" to msg (msg must be a string expression) */
#define FSS_PERROR(comp, msg) \
    do { \
        int _fss_saved_errno = errno; \
        std::string _fss_perror_msg = (msg); \
        FSS_LOG_ERROR((comp), _fss_perror_msg << ": " << std::strerror(_fss_saved_errno)); \
    } while (false)
