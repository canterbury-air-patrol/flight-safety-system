#ifdef HAVE_CATCH2_CATCH_ALL_HPP
#include <catch2/catch_all.hpp>
#elif HAVE_CATCH2_CATCH_HPP
#include <catch2/catch.hpp>
#elif HAVE_CATCH_CATCH_HPP
#include <catch/catch.hpp>
#elif HAVE_CATCH_HPP
#include <catch.hpp>
#else
#error No catch header
#endif

#include <atomic>
#include <cerrno>
#include <regex>
#include <thread>
#include <vector>

#include "fss-log.hpp"
#include "test_helpers.hpp"

TEST_CASE("log: level filter suppresses lower-severity lines")
{
    fss_test::capture_cerr cap;

    fss_test::scoped_log_level guard("error");
    FSS_LOG_ERROR("t", "err-visible");
    FSS_LOG_WARN("t", "warn-hidden");
    FSS_LOG_INFO("t", "info-hidden");
    FSS_LOG_DEBUG("t", "debug-hidden");

    auto out = cap.str();
    REQUIRE(out.find("err-visible") != std::string::npos);
    REQUIRE(out.find("warn-hidden") == std::string::npos);
    REQUIRE(out.find("info-hidden") == std::string::npos);
    REQUIRE(out.find("debug-hidden") == std::string::npos);
}

TEST_CASE("log: debug level lets every severity through")
{
    fss_test::capture_cerr cap;

    fss_test::scoped_log_level guard("debug");
    FSS_LOG_ERROR("t", "e");
    FSS_LOG_WARN("t", "w");
    FSS_LOG_INFO("t", "i");
    FSS_LOG_DEBUG("t", "d");

    auto out = cap.str();
    REQUIRE(out.find("[ERROR]") != std::string::npos);
    REQUIRE(out.find("[WARN ]") != std::string::npos);
    REQUIRE(out.find("[INFO ]") != std::string::npos);
    REQUIRE(out.find("[DEBUG]") != std::string::npos);
}

TEST_CASE("log: FSS_PERROR appends strerror")
{
    fss_test::capture_cerr cap;
    fss_test::scoped_log_level guard("error");

    errno = EACCES;
    FSS_PERROR("t", "open config");

    auto out = cap.str();
    REQUIRE(out.find("open config") != std::string::npos);
    REQUIRE(out.find("Permission denied") != std::string::npos);
}

TEST_CASE("log: emitted lines match expected format")
{
    fss_test::capture_cerr cap;
    fss_test::scoped_log_level guard("info");

    FSS_LOG_INFO("component-name", "hello world");

    auto out = cap.str();
    std::regex pattern(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z \[INFO \] \[component-name\] hello world\n$)");
    REQUIRE(std::regex_match(out, pattern));
}

TEST_CASE("log: concurrent writers produce no interleaved lines")
{
    fss_test::capture_cerr cap;
    fss_test::scoped_log_level guard("info");

    constexpr int thread_count = 8;
    constexpr int lines_per_thread = 1000;

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (int t = 0; t < thread_count; ++t)
    {
        workers.emplace_back([t, &start]() {
            while (!start.load())
            {
                std::this_thread::yield();
            }
            for (int i = 0; i < lines_per_thread; ++i)
            {
                FSS_LOG_INFO("thr", "tid=" << t << " i=" << i);
            }
        });
    }
    start.store(true);
    for (auto &w : workers)
    {
        w.join();
    }

    auto out = cap.str();
    std::regex line(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z \[INFO \] \[thr\] tid=\d+ i=\d+$)");

    size_t start_off = 0;
    size_t count = 0;
    while (start_off < out.size())
    {
        size_t nl = out.find('\n', start_off);
        if (nl == std::string::npos)
        {
            break;
        }
        std::string ln = out.substr(start_off, nl - start_off);
        REQUIRE(std::regex_match(ln, line));
        ++count;
        start_off = nl + 1;
    }
    REQUIRE(count == static_cast<size_t>(thread_count * lines_per_thread));
}

TEST_CASE("log: set_level covers all four string variants")
{
    namespace fss_log = flight_safety_system::log;

    fss_test::scoped_log_level guard("info");

    fss_log::set_level("warn");
    REQUIRE(fss_log::detail::current_level().load() == fss_log::level::Warn);

    fss_log::set_level("error");
    REQUIRE(fss_log::detail::current_level().load() == fss_log::level::Error);

    fss_log::set_level("info");
    REQUIRE(fss_log::detail::current_level().load() == fss_log::level::Info);

    fss_log::set_level("debug");
    REQUIRE(fss_log::detail::current_level().load() == fss_log::level::Debug);
}

TEST_CASE("log: level_str returns expected tag for each level")
{
    namespace fss_log = flight_safety_system::log;

    REQUIRE(std::string(fss_log::detail::level_str(fss_log::level::Error)) == "ERROR");
    REQUIRE(std::string(fss_log::detail::level_str(fss_log::level::Warn)) == "WARN ");
    REQUIRE(std::string(fss_log::detail::level_str(fss_log::level::Info)) == "INFO ");
    REQUIRE(std::string(fss_log::detail::level_str(fss_log::level::Debug)) == "DEBUG");
    /* Unreachable in normal usage but exercises the default return path. */
    REQUIRE(std::string(fss_log::detail::level_str(static_cast<fss_log::level>(999))) == "?????");
}
