#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace flight_safety_system {
namespace log {

enum class level : int {
    Error = 0,
    Warn = 1,
    Info = 2,
    Debug = 3,
};

namespace detail {

inline std::atomic<level> &current_level()
{
    static std::atomic<level> lvl{[]() -> level {
        level l = level::Info;
        const char *env = std::getenv("FSS_LOG_LEVEL");
        if (env == nullptr)
        {
            return l;
        }
        std::string s{env};
        if (s == "error" || s == "ERROR")
        {
            l = level::Error;
        }
        else if (s == "warn" || s == "WARN")
        {
            l = level::Warn;
        }
        else if (s == "info" || s == "INFO")
        {
            l = level::Info;
        }
        else if (s == "debug" || s == "DEBUG")
        {
            l = level::Debug;
        }
        return l;
    }()};
    return lvl;
}

inline std::mutex &log_mutex()
{
    static std::mutex m;
    return m;
}

inline const char *level_str(level lvl)
{
    switch (lvl)
    {
        case level::Error: return "ERROR";
        case level::Warn: return "WARN ";
        case level::Info: return "INFO ";
        case level::Debug: return "DEBUG";
    }
    return "?????";
}

} // namespace detail

inline void set_level(const std::string &lvl_str)
{
    if (lvl_str == "error" || lvl_str == "ERROR")
    {
        detail::current_level() = level::Error;
    }
    else if (lvl_str == "warn" || lvl_str == "WARN")
    {
        detail::current_level() = level::Warn;
    }
    else if (lvl_str == "info" || lvl_str == "INFO")
    {
        detail::current_level() = level::Info;
    }
    else if (lvl_str == "debug" || lvl_str == "DEBUG")
    {
        detail::current_level() = level::Debug;
    }
}

inline void write(level lvl, const char *component, const std::string &msg)
{
    std::scoped_lock guard(detail::log_mutex());
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[21]{};
    struct tm gm_tm{};
    gmtime_r(&t, &gm_tm);
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &gm_tm);
    std::cerr << buf << " [" << detail::level_str(lvl) << "]"
              << " [" << component << "] " << msg << "\n";
}

} // namespace log
} // namespace flight_safety_system

/* Stream-expression logging: FSS_LOG_ERROR("comp", "val=" << val) */
#define FSS_LOG(lvl, comp, ...)                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        auto _fss_log_lvl = (lvl);                                                                                     \
        if (_fss_log_lvl <= flight_safety_system::log::detail::current_level())                                        \
        {                                                                                                              \
            std::ostringstream _fss_log_oss;                                                                           \
            _fss_log_oss << __VA_ARGS__;                                                                               \
            flight_safety_system::log::write(_fss_log_lvl, (comp), _fss_log_oss.str());                                \
        }                                                                                                              \
    } while (false)

#define FSS_LOG_ERROR(comp, ...) FSS_LOG(flight_safety_system::log::level::Error, (comp), __VA_ARGS__)
#define FSS_LOG_WARN(comp, ...) FSS_LOG(flight_safety_system::log::level::Warn, (comp), __VA_ARGS__)
#define FSS_LOG_INFO(comp, ...) FSS_LOG(flight_safety_system::log::level::Info, (comp), __VA_ARGS__)
#define FSS_LOG_DEBUG(comp, ...) FSS_LOG(flight_safety_system::log::level::Debug, (comp), __VA_ARGS__)

/* perror() replacement: appends ": strerror(errno)" to msg (msg must be a string expression) */
#define FSS_PERROR(comp, msg)                                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        int _fss_saved_errno = errno;                                                                                  \
        std::string _fss_perror_msg = (msg);                                                                           \
        FSS_LOG_ERROR((comp), _fss_perror_msg << ": " << std::strerror(_fss_saved_errno));                             \
    } while (false)

namespace flight_safety_system {

/* Runs a unit of work and isolates exceptions so a long-running loop is not
 * torn down by one failure. Consecutive failures are logged with throttling
 * (the first, then every log_interval-th) so a persistent fault does not flood
 * the log, and a single recovery line is logged once work succeeds again. The
 * loop is never stopped — the caller keeps running. */
class exception_guard {
    const char *category;
    const char *label;
    uint64_t consecutive_failures{0};
    static constexpr uint64_t log_interval = 100;

    void note_failure(const std::string &what)
    {
        this->consecutive_failures++;
        if (this->consecutive_failures == 1 || (this->consecutive_failures % log_interval) == 0)
        {
            FSS_LOG_ERROR(this->category, "Exception in " << this->label << " (" << this->consecutive_failures
                                                          << " consecutive): " << what);
        }
    }
public:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    exception_guard(const char *t_category, const char *t_label) : category(t_category), label(t_label) {}
    exception_guard(const exception_guard &) = delete;
    exception_guard(exception_guard &&) = delete;
    auto operator=(const exception_guard &) -> exception_guard & = delete;
    auto operator=(exception_guard &&) -> exception_guard & = delete;

    template<typename F> void run(F &&work)
    {
        try
        {
            std::forward<F>(work)();
            if (this->consecutive_failures > 0)
            {
                FSS_LOG_INFO(this->category, this->label << " recovered after " << this->consecutive_failures
                                                         << " consecutive failure(s)");
                this->consecutive_failures = 0;
            }
        }
        catch (const std::exception &e)
        {
            this->note_failure(e.what());
        }
        catch (...)
        {
            this->note_failure("unknown exception");
        }
    }
};

} // namespace flight_safety_system
