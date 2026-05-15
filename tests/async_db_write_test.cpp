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
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "db-write-queue.hpp"
#include "fss-log.hpp"
#include "test_helpers.hpp"

namespace fss = flight_safety_system;

namespace {

/* Thread-safe capture of writes. Stores asset_id + rtt_ms (for rtt_write)
 * or just asset_id for other variants, since these tests only care about
 * ordering and counts. */
struct CapturingSink {
    mutable std::mutex mtx{};
    std::vector<uint64_t> asset_ids{};
    std::atomic<uint64_t> count{0};
    std::chrono::milliseconds delay{0};

    void operator()(const fss::server::db_write_task &task)
    {
        if (delay.count() > 0)
        {
            std::this_thread::sleep_for(delay);
        }
        std::lock_guard<std::mutex> guard(mtx);
        std::visit(fss::server::overloaded{
                       [&](const fss::server::rtt_write &w) -> void { asset_ids.push_back(w.asset_id); },
                       [&](const fss::server::position_write &w) -> void { asset_ids.push_back(w.asset_id); },
                       [&](const fss::server::status_write &w) -> void { asset_ids.push_back(w.asset_id); },
                       [&](const fss::server::search_status_write &w) -> void { asset_ids.push_back(w.asset_id); },
                   },
                   task);
        count.fetch_add(1);
    }

    auto snapshot() const -> std::vector<uint64_t>
    {
        std::lock_guard<std::mutex> guard(mtx);
        return asset_ids;
    }
};

} // namespace

TEST_CASE("db_write_queue: enqueue-then-drain preserves order")
{
    auto cap = std::make_shared<CapturingSink>();
    fss::server::db_write_queue q(100, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); });

    for (uint64_t i = 1; i <= 20; ++i)
    {
        q.enqueue(fss::server::rtt_write{i, i * 10});
    }

    REQUIRE(fss_test::wait_for([&]() -> bool { return cap->count.load() == 20; }));
    q.stop();

    auto seen = cap->snapshot();
    REQUIRE(seen.size() == 20);
    for (size_t i = 0; i < seen.size(); ++i)
    {
        REQUIRE(seen[i] == i + 1);
    }
    REQUIRE(q.dropped_count() == 0);
}

TEST_CASE("db_write_queue: bounded queue drops oldest when full")
{
    /* Slow sink so we can overflow the queue deterministically. */
    auto cap = std::make_shared<CapturingSink>();
    cap->delay = std::chrono::milliseconds(50);

    constexpr std::size_t depth = 5;
    fss::server::db_write_queue q(depth, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); });

    /* Worker will pick up task 1 and sleep. Fill 5 slots, then push 10 more;
     * older entries beyond depth must be dropped. */
    for (uint64_t i = 1; i <= 20; ++i)
    {
        q.enqueue(fss::server::rtt_write{i, 0});
    }

    REQUIRE(q.dropped_count() > 0);

    /* Stop waits for the worker to drain whatever's left. */
    q.stop();

    auto seen = cap->snapshot();
    /* Surviving entries must be monotonically increasing (drop-oldest). */
    for (size_t i = 1; i < seen.size(); ++i)
    {
        REQUIRE(seen[i] > seen[i - 1]);
    }
    /* Last entry surviving must be the most recent producer pushed (20). */
    REQUIRE(seen.back() == 20);
}

TEST_CASE("db_write_queue: stop drains pending work")
{
    auto cap = std::make_shared<CapturingSink>();
    cap->delay = std::chrono::milliseconds(5);

    fss::server::db_write_queue q(1000, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); });
    for (uint64_t i = 1; i <= 50; ++i)
    {
        q.enqueue(fss::server::position_write{i, 0.0, 0.0, 0});
    }

    q.stop();

    REQUIRE(cap->count.load() == 50);
    REQUIRE(q.pending_count() == 0);
}

TEST_CASE("db_write_queue: stop is idempotent")
{
    auto cap = std::make_shared<CapturingSink>();
    fss::server::db_write_queue q(100, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); });
    q.enqueue(fss::server::rtt_write{1, 1});
    REQUIRE(fss_test::wait_for([&]() -> bool { return cap->count.load() == 1; }));
    q.stop();
    q.stop();
    /* Destructor also calls stop — must not deadlock / double-join. */
}

TEST_CASE("db_write_queue: enqueue after stop is a no-op")
{
    auto cap = std::make_shared<CapturingSink>();
    fss::server::db_write_queue q(100, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); });

    q.enqueue(fss::server::rtt_write{1, 10});
    q.enqueue(fss::server::rtt_write{2, 20});
    REQUIRE(fss_test::wait_for([&]() -> bool { return cap->count.load() == 2; }));

    q.stop();
    const auto dropped_after_stop = q.dropped_count();
    const auto pending_after_stop = q.pending_count();

    for (uint64_t i = 3; i <= 10; ++i)
    {
        q.enqueue(fss::server::rtt_write{i, 0});
    }

    REQUIRE(q.pending_count() == pending_after_stop);
    REQUIRE(q.dropped_count() == dropped_after_stop);
    REQUIRE(cap->count.load() == 2);
}

TEST_CASE("db_write_queue: slow sink does not block producers")
{
    auto cap = std::make_shared<CapturingSink>();
    cap->delay = std::chrono::milliseconds(200);

    fss::server::db_write_queue q(10000, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); });

    auto start = std::chrono::steady_clock::now();
    for (uint64_t i = 1; i <= 20; ++i)
    {
        q.enqueue(fss::server::rtt_write{i, 0});
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    /* Enqueues must not block waiting on the sink. Use a generous bound so
     * the test doesn't flake on loaded CI while still catching obviously
     * blocking behavior. */
    REQUIRE(elapsed < std::chrono::seconds(1));

    /* The slow sink should not have processed many items yet. */
    REQUIRE(cap->count.load() < 5);

    REQUIRE(fss_test::wait_for([&]() -> bool { return cap->count.load() == 20; }, std::chrono::milliseconds(6000)));
    q.stop();
}

TEST_CASE("db_write_queue: drop log message appears on stderr when queue fills")
{
    auto cap = std::make_shared<CapturingSink>();
    cap->delay = std::chrono::milliseconds(50);

    /* Earlier tests may have left level at ERROR; ensure WARN is visible. */
    flight_safety_system::log::set_level("warn");
    fss_test::capture_cerr cerr_capture;

    {
        fss::server::db_write_queue q(3, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); });
        for (uint64_t i = 1; i <= 20; ++i)
        {
            q.enqueue(fss::server::rtt_write{i, 0});
        }
        /* Let the drop happen; worker is slow so all overflow drops occur
         * during the enqueue loop. */
        q.stop();
    }

    auto out = cerr_capture.str();
    REQUIRE(out.find("db-writer") != std::string::npos);
    REQUIRE(out.find("dropped") != std::string::npos);
    flight_safety_system::log::set_level("info");
}

TEST_CASE("db_write_queue: write_failure_count tracks sink exceptions")
{
    constexpr uint64_t task_count = 5;
    auto throwing_sink = [](const fss::server::db_write_task &) -> void {
        throw std::runtime_error("injected sink failure");
    };

    fss::server::db_write_queue q(100, throwing_sink);
    for (uint64_t i = 1; i <= task_count; ++i)
    {
        q.enqueue(fss::server::rtt_write{i, 0});
    }

    REQUIRE(fss_test::wait_for([&]() -> bool { return q.write_failure_count() == task_count; }));
    q.stop();

    REQUIRE(q.write_failure_count() == task_count);
}

TEST_CASE("db_write_queue: destructor without explicit stop drains pending work")
{
    auto cap = std::make_shared<CapturingSink>();
    cap->delay = std::chrono::milliseconds(5);

    {
        fss::server::db_write_queue q(1000, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); });
        for (uint64_t i = 1; i <= 30; ++i)
        {
            q.enqueue(fss::server::search_status_write{i, 0, 0, 0});
        }
        /* q goes out of scope; destructor must drain + join. */
    }

    REQUIRE(cap->count.load() == 30);
}
