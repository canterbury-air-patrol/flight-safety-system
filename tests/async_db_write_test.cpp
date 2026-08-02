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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "db-write-queue.hpp"
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
    /* Optional gate: while closed, the worker parks at the start of the sink
     * (after recording that it entered) so a test can hold the queue in a known
     * state. Open by default, so existing tests are unaffected. */
    std::mutex gate_mtx{};
    std::condition_variable gate_cv{};
    bool gate_open{true};
    std::atomic<uint64_t> entered{0};

    void open_gate()
    {
        {
            std::lock_guard<std::mutex> guard(gate_mtx);
            gate_open = true;
        }
        gate_cv.notify_all();
    }

    void operator()(const fss::server::db_write_task &task)
    {
        entered.fetch_add(1);
        {
            std::unique_lock<std::mutex> gate(gate_mtx);
            gate_cv.wait(gate, [this]() -> bool { return gate_open; });
        }
        if (delay.count() > 0)
        {
            std::this_thread::sleep_for(delay);
        }
        std::lock_guard<std::mutex> guard(mtx);
        std::visit(
            fss::server::overloaded{
                [&](const fss::server::rtt_write &w) -> void { asset_ids.push_back(w.asset_id); },
                [&](const fss::server::position_write &w) -> void { asset_ids.push_back(w.asset_id); },
                [&](const fss::server::status_write &w) -> void { asset_ids.push_back(w.asset_id); },
                [&](const fss::server::search_status_write &w) -> void { asset_ids.push_back(w.asset_id); },
                /* Command writes are keyed by the command row id, not asset
                 * id; these tests don't enqueue them, but the visit must
                 * cover every alternative, so record the id present. */
                [&](const fss::server::command_dispatch_write &w) -> void { asset_ids.push_back(w.command_dbid); },
                [&](const fss::server::command_ack_write &w) -> void { asset_ids.push_back(w.command_dbid); },
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

/* Every case that does not exercise the probe itself passes this. The
 * constructor requires a probe function (todo/78) precisely so a server wired
 * without one — which could never leave the degraded state — fails to compile
 * rather than shipping. */
auto healthy_probe() -> bool
{
    return true;
}

} // namespace

TEST_CASE("db_write_queue: enqueue-then-drain preserves order")
{
    auto cap = std::make_shared<CapturingSink>();
    fss::server::db_write_queue q(
        100, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); }, healthy_probe);

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
    fss::server::db_write_queue q(
        depth, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); }, healthy_probe);

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

    fss::server::db_write_queue q(
        1000, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); }, healthy_probe);
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
    fss::server::db_write_queue q(
        100, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); }, healthy_probe);
    q.enqueue(fss::server::rtt_write{1, 1});
    REQUIRE(fss_test::wait_for([&]() -> bool { return cap->count.load() == 1; }));
    q.stop();
    q.stop();
    /* Destructor also calls stop — must not deadlock / double-join. */
}

TEST_CASE("db_write_queue: enqueue after stop is a no-op")
{
    auto cap = std::make_shared<CapturingSink>();
    fss::server::db_write_queue q(
        100, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); }, healthy_probe);

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

    fss::server::db_write_queue q(
        10000, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); }, healthy_probe);

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

    /* The drop notice is logged at WARN. */
    fss_test::scoped_log_level guard("warn");
    fss_test::capture_cerr cerr_capture;

    {
        fss::server::db_write_queue q(
            3, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); }, healthy_probe);
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
}

TEST_CASE("db_write_queue: a command dispatch survives a telemetry overflow")
{
    /* todo/20: telemetry pressure must never evict a queued command write. Fill
     * the queue with telemetry, slip in a command dispatch, then bury it under
     * far more telemetry. The command must still reach the sink, and no command
     * may be counted as dropped. */
    auto cap = std::make_shared<CapturingSink>();
    cap->delay = std::chrono::milliseconds(50); /* slow sink so the queue stays full */

    constexpr std::size_t depth = 5;
    fss::server::db_write_queue q(
        depth, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); }, healthy_probe);

    constexpr uint64_t cmd_id = 1000000; /* distinct from any telemetry asset id below */
    for (uint64_t i = 1; i <= depth; ++i)
    {
        q.enqueue(fss::server::rtt_write{i, 0});
    }
    q.enqueue(fss::server::command_dispatch_write{cmd_id, 7});
    for (uint64_t i = 100; i < 200; ++i)
    {
        q.enqueue(fss::server::rtt_write{i, 0});
    }

    q.stop(); /* drains remaining work */

    auto seen = cap->snapshot();
    REQUIRE(std::find(seen.begin(), seen.end(), cmd_id) != seen.end());
    REQUIRE(q.command_dropped_count() == 0);
}

TEST_CASE("db_write_queue: a command ack survives a telemetry overflow")
{
    /* todo/20: same protection for command acks — losing one drops the recorded
     * outcome of a dispatched command. */
    auto cap = std::make_shared<CapturingSink>();
    cap->delay = std::chrono::milliseconds(50);

    constexpr std::size_t depth = 5;
    fss::server::db_write_queue q(
        depth, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); }, healthy_probe);

    constexpr uint64_t ack_command_dbid = 2000000;
    for (uint64_t i = 1; i <= depth; ++i)
    {
        q.enqueue(fss::server::position_write{i, 0.0, 0.0, 0});
    }
    q.enqueue(fss::server::command_ack_write{ack_command_dbid, 0, 0, 0});
    for (uint64_t i = 100; i < 200; ++i)
    {
        q.enqueue(fss::server::position_write{i, 0.0, 0.0, 0});
    }

    q.stop();

    auto seen = cap->snapshot();
    REQUIRE(std::find(seen.begin(), seen.end(), ack_command_dbid) != seen.end());
    REQUIRE(q.command_dropped_count() == 0);
}

TEST_CASE("db_write_queue: command overload drops on a distinct command-specific path")
{
    /* todo/20: if the queue fills entirely with command writes (genuine command
     * overload, not telemetry pressure) a command may have to be dropped to stay
     * bounded — but on a distinct, observable error path, never the generic
     * telemetry drop counter/log. */
    auto cap = std::make_shared<CapturingSink>();
    cap->delay = std::chrono::milliseconds(50);

    fss_test::scoped_log_level guard("error");
    fss_test::capture_cerr cerr_capture;

    {
        constexpr std::size_t depth = 3;
        fss::server::db_write_queue q(
            depth, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); }, healthy_probe);
        for (uint64_t i = 1; i <= 10; ++i)
        {
            q.enqueue(fss::server::command_dispatch_write{i, i});
        }
        q.stop();

        REQUIRE(q.command_dropped_count() > 0);
        /* The telemetry drop path must not have been used. */
        REQUIRE(q.dropped_count() == 0);
    }

    auto out = cerr_capture.str();
    REQUIRE(out.find("db-writer") != std::string::npos);
    REQUIRE(out.find("command") != std::string::npos);
}

TEST_CASE("db_write_queue: telemetry is rejected without evicting a command when the queue is full of commands")
{
    /* todo/20: the complement of the eviction tests. When the queue is full of
     * protected command writes and a telemetry task arrives, there is nothing to
     * evict — the telemetry must be rejected outright, never displacing a
     * command. Gate the sink so the queue can be held full deterministically. */
    auto cap = std::make_shared<CapturingSink>();
    cap->gate_open = false; /* the worker will park on the first task */

    constexpr std::size_t depth = 3;
    fss::server::db_write_queue q(
        depth, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); }, healthy_probe);

    /* Park the worker on a first command so the remaining slots fill predictably. */
    q.enqueue(fss::server::command_dispatch_write{1, 1});
    REQUIRE(fss_test::wait_for([&]() -> bool { return cap->entered.load() >= 1; }));

    /* Fill the queue to capacity with commands — reaching depth exactly drops
     * nothing. */
    for (uint64_t i = 2; i <= depth + 1; ++i)
    {
        q.enqueue(fss::server::command_dispatch_write{i, i});
    }
    REQUIRE(q.pending_count() == depth);
    REQUIRE(q.command_dropped_count() == 0);
    REQUIRE(q.dropped_count() == 0);

    /* Telemetry arrives with no telemetry to evict: it is rejected, and no
     * command is dropped to make room. */
    constexpr uint64_t telemetry_id = 9999;
    q.enqueue(fss::server::rtt_write{telemetry_id, 0});
    REQUIRE(q.command_dropped_count() == 0);
    REQUIRE(q.dropped_count() == 1);
    REQUIRE(q.pending_count() == depth);

    /* Release the worker and drain: the rejected telemetry was never recorded. */
    cap->open_gate();
    q.stop();
    auto seen = cap->snapshot();
    REQUIRE(std::find(seen.begin(), seen.end(), telemetry_id) == seen.end());
}

TEST_CASE("db_write_queue: write_failure_count tracks sink exceptions")
{
    constexpr uint64_t task_count = 5;
    auto throwing_sink = [](const fss::server::db_write_task &) -> void {
        throw std::runtime_error("injected sink failure");
    };

    fss::server::db_write_queue q(100, throwing_sink, healthy_probe);
    for (uint64_t i = 1; i <= task_count; ++i)
    {
        q.enqueue(fss::server::rtt_write{i, 0});
    }

    REQUIRE(fss_test::wait_for([&]() -> bool { return q.write_failure_count() == task_count; }));
    q.stop();

    REQUIRE(q.write_failure_count() == task_count);
}

TEST_CASE("db_write_queue: catch-all handler counts non-exception throws")
{
    auto weird_sink = [](const fss::server::db_write_task &) -> void { throw 42; };

    fss::server::db_write_queue q(100, weird_sink, healthy_probe);
    q.enqueue(fss::server::rtt_write{1, 0});

    REQUIRE(fss_test::wait_for([&]() -> bool { return q.write_failure_count() == 1; }));
    q.stop();

    REQUIRE(q.write_failure_count() == 1);
}

TEST_CASE("db_write_queue: destructor without explicit stop drains pending work")
{
    auto cap = std::make_shared<CapturingSink>();
    cap->delay = std::chrono::milliseconds(5);

    {
        fss::server::db_write_queue q(
            1000, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); }, healthy_probe);
        for (uint64_t i = 1; i <= 30; ++i)
        {
            q.enqueue(fss::server::search_status_write{i, 0, 0, 0});
        }
        /* q goes out of scope; destructor must drain + join. */
    }

    REQUIRE(cap->count.load() == 30);
}

/* ── The fail-safe health probe (todo/78) ────────────────────────────────── */

TEST_CASE("db_write_queue: a probe runs only once the queue has drained")
{
    /* The probe is the fail-safe's evidence that the write path works *now*,
     * so it must describe the state after the backlog cleared, not race it.
     * It is also deliberately not a queued task: a queued probe would make
     * pending_count() non-zero, which the fail-safe reads as "not drained" —
     * recovery would become unreachable and the fleet would stay severed. */
    auto cap = std::make_shared<CapturingSink>();
    {
        std::lock_guard<std::mutex> guard(cap->gate_mtx);
        cap->gate_open = false;
    }
    std::atomic<uint64_t> probes{0};
    std::atomic<uint64_t> writes_seen_by_probe{0};
    fss::server::db_write_queue q(
        100, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); },
        [&]() -> bool {
            writes_seen_by_probe.store(cap->count.load());
            probes.fetch_add(1);
            return true;
        });

    constexpr uint64_t task_count = 5;
    for (uint64_t i = 1; i <= task_count; ++i)
    {
        q.enqueue(fss::server::rtt_write{i, i});
    }
    /* The worker is parked inside the sink on the first task; the rest sit in
     * the deque. */
    REQUIRE(fss_test::wait_for([&]() -> bool { return cap->entered.load() >= 1; }));
    const std::size_t pending_before = q.pending_count();
    q.requestProbe();
    /* Asking for a probe must not change the backlog the fail-safe measures. */
    REQUIRE(q.pending_count() == pending_before);
    REQUIRE(probes.load() == 0);

    cap->open_gate();
    REQUIRE(fss_test::wait_for([&]() -> bool { return probes.load() == 1; }));
    q.stop();

    REQUIRE(q.pending_count() == 0);
    REQUIRE(writes_seen_by_probe.load() == task_count);
    REQUIRE(q.probe_success_count() == 1);
}

TEST_CASE("db_write_queue: repeated probe requests coalesce into one probe")
{
    auto cap = std::make_shared<CapturingSink>();
    {
        std::lock_guard<std::mutex> guard(cap->gate_mtx);
        cap->gate_open = false;
    }
    std::atomic<uint64_t> probes{0};
    fss::server::db_write_queue q(
        100, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); },
        [&]() -> bool {
            probes.fetch_add(1);
            return true;
        });

    q.enqueue(fss::server::rtt_write{1, 1});
    REQUIRE(fss_test::wait_for([&]() -> bool { return cap->entered.load() >= 1; }));
    /* The caller asks once a second while degraded and never waits for an
     * answer, so requests pile up behind a slow write; at most one probe may
     * ever be outstanding. */
    for (int i = 0; i < 20; i++)
    {
        q.requestProbe();
    }

    cap->open_gate();
    REQUIRE(fss_test::wait_for([&]() -> bool { return probes.load() >= 1; }));
    q.stop();

    REQUIRE(probes.load() == 1);
    REQUIRE(q.probe_success_count() == 1);
}

TEST_CASE("db_write_queue: a successful probe counts as a success, not a write")
{
    fss::server::db_write_queue q(100, [](const fss::server::db_write_task &) -> void {}, healthy_probe);
    q.requestProbe();

    REQUIRE(fss_test::wait_for([&]() -> bool { return q.probe_success_count() == 1; }));
    q.stop();

    REQUIRE(q.probe_success_count() == 1);
    REQUIRE(q.probe_failure_count() == 0);
    REQUIRE(q.probe_inconclusive_count() == 0);
    REQUIRE(q.write_failure_count() == 0);
}

TEST_CASE("db_write_queue: a throwing probe counts as a probe failure, not a write failure")
{
    /* Kept off write_failure_count() on purpose: the caller logs that counter at
     * ERROR on every change, so routing probe failures through it would emit one
     * ERROR per second forever against a permanently dead database. */
    fss::server::db_write_queue q(
        100, [](const fss::server::db_write_task &) -> void {},
        []() -> bool { throw std::runtime_error("probe boom"); });
    q.requestProbe();

    REQUIRE(fss_test::wait_for([&]() -> bool { return q.probe_failure_count() == 1; }));
    q.stop();

    REQUIRE(q.probe_failure_count() == 1);
    REQUIRE(q.probe_success_count() == 0);
    REQUIRE(q.write_failure_count() == 0);
    REQUIRE(q.last_probe_error() == "probe boom");
}

TEST_CASE("db_write_queue: an inconclusive probe is neither a success nor a failure")
{
    /* An empty asset table makes the probe statement match no row: it fires no
     * trigger and extends no heap page, so it proves nothing. Counting it as a
     * success would readmit the fleet on the strength of a statement that did
     * nothing. */
    fss::server::db_write_queue q(
        100, [](const fss::server::db_write_task &) -> void {}, []() -> bool { return false; });
    q.requestProbe();

    REQUIRE(fss_test::wait_for([&]() -> bool { return q.probe_inconclusive_count() == 1; }));
    q.stop();

    REQUIRE(q.probe_inconclusive_count() == 1);
    REQUIRE(q.probe_success_count() == 0);
    REQUIRE(q.probe_failure_count() == 0);
}

TEST_CASE("db_write_queue: stop with a probe outstanding does not hang")
{
    /* Shutdown must never wait on the database: an outstanding probe is
     * abandoned, not run, once stopping is set and the queue is empty. */
    auto cap = std::make_shared<CapturingSink>();
    {
        std::lock_guard<std::mutex> guard(cap->gate_mtx);
        cap->gate_open = false;
    }
    std::atomic<uint64_t> probes{0};
    {
        fss::server::db_write_queue q(
            100, [cap](const fss::server::db_write_task &t) -> void { (*cap)(t); },
            [&]() -> bool {
                probes.fetch_add(1);
                return true;
            });

        q.enqueue(fss::server::rtt_write{1, 1});
        REQUIRE(fss_test::wait_for([&]() -> bool { return cap->entered.load() >= 1; }));
        q.requestProbe();

        std::thread releaser([cap]() -> void {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            cap->open_gate();
        });
        q.stop();
        releaser.join();
        /* Destructor runs on a stopped queue: stop() is idempotent. */
    }

    REQUIRE(probes.load() == 0);
}

TEST_CASE("db_write_queue: a probe requested after stop is ignored")
{
    std::atomic<uint64_t> probes{0};
    fss::server::db_write_queue q(
        100, [](const fss::server::db_write_task &) -> void {},
        [&]() -> bool {
            probes.fetch_add(1);
            return true;
        });
    q.stop();
    q.requestProbe();

    REQUIRE(probes.load() == 0);
    REQUIRE(q.probe_success_count() == 0);
}
