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
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "db-write-queue.hpp"
#include "server-failsafe.hpp"
#include "test_helpers.hpp"

namespace fss = flight_safety_system;

using fss::server::db_failsafe;

/* Unit tests for the unified DB fail-safe monitor (todo/45 + todo/47): the
 * write-failure incident semantics carried over from todo/34's tracker, the
 * immediate command-drop trip, and the latched degraded state with its
 * drain-plus-quiet-window recovery rule. */

TEST_CASE("db_failsafe: a single transient write failure never trips")
{
    /* Pins the documented todo/34 intent ("A single transient failure never
     * trips this"). The pre-todo/47 tracker violated it whenever the recovery
     * grace exceeded the disconnect threshold — the default configuration —
     * because a lone failure kept the incident 'active' through the grace
     * period and the age check then fired with no further failure. */
    db_failsafe f(5, 15);
    REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::none);
    for (int i = 0; i < 30; i++)
    {
        REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::none);
        REQUIRE(!f.degraded());
    }
}

TEST_CASE("db_failsafe: sustained write failures trip once they span the age threshold")
{
    db_failsafe f(5, 15);
    uint64_t failures = 0;
    /* Failures every tick: the incident starts at the first one; the trip
     * needs a new failure arriving at age >= 5, i.e. the sixth tick. */
    for (int age = 0; age < 5; age++)
    {
        REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::none);
        REQUIRE(!f.degraded());
    }
    REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::tripped);
    REQUIRE(f.degraded());
    REQUIRE(f.reason() == db_failsafe::trip_reason::write_failure);
}

TEST_CASE("db_failsafe: failures recurring within the grace window chain into one incident")
{
    /* Real telemetry lands every ~5s, so failures arrive in bursts with quiet
     * ticks between: gaps shorter than the recovery grace must not restart
     * the incident age (the reason todo/34 replaced a consecutive-tick
     * counter with a wall-clock incident). */
    db_failsafe f(4, 15);
    REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::none); // incident starts
    for (int i = 0; i < 3; i++)
    {
        REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::none); // quiet gap < grace
    }
    /* A new failure at age 4 >= threshold 4: same incident, trips. */
    REQUIRE(f.tick(2, 0, 0) == db_failsafe::event::tripped);
    REQUIRE(f.reason() == db_failsafe::trip_reason::write_failure);
}

TEST_CASE("db_failsafe: a gap longer than the grace window starts a fresh incident")
{
    db_failsafe f(5, 3);
    REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::none);
    for (int i = 0; i < 6; i++)
    {
        REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::none); // ages out after grace
    }
    /* Next failure is a fresh incident at age 0, despite the total span since
     * the first failure being well past the threshold. */
    REQUIRE(f.tick(2, 0, 0) == db_failsafe::event::none);
    REQUIRE(!f.degraded());
}

TEST_CASE("db_failsafe: a disconnect threshold of 0 trips on the first failure")
{
    db_failsafe f(0, 15);
    REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::tripped);
    REQUIRE(f.degraded());
    REQUIRE(f.reason() == db_failsafe::trip_reason::write_failure);
}

TEST_CASE("db_failsafe: any command drop trips immediately")
{
    /* todo/45: a dropped command dispatch/ack write is known-destroyed audit
     * state; there is no threshold to age through. */
    db_failsafe f(5, 15);
    REQUIRE(f.tick(0, 0, 0) == db_failsafe::event::none);
    REQUIRE(f.tick(0, 1, 0) == db_failsafe::event::tripped);
    REQUIRE(f.degraded());
    REQUIRE(f.reason() == db_failsafe::trip_reason::command_drop);
}

TEST_CASE("db_failsafe: no recovery while the write queue still holds a backlog")
{
    db_failsafe f(0, 2);
    REQUIRE(f.tick(1, 0, 5) == db_failsafe::event::tripped);
    /* Quiet, but the backlog never drains: stays degraded no matter how long
     * the quiet window grows (a wedged-but-silent sink is not health). */
    for (int i = 0; i < 10; i++)
    {
        REQUIRE(f.tick(1, 0, 5) == db_failsafe::event::none);
        REQUIRE(f.degraded());
    }
    /* Drain completes and the quiet window is already satisfied: recovered. */
    REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::recovered);
    REQUIRE(!f.degraded());
    REQUIRE(f.reason() == db_failsafe::trip_reason::none);
}

TEST_CASE("db_failsafe: recovery requires the full quiet window after the last failure")
{
    db_failsafe f(0, 3);
    REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::tripped);
    /* Quiet, drained ticks: the window must strictly exceed the grace. */
    REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::none); // 1s quiet
    REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::none); // 2s quiet
    REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::none); // 3s quiet
    REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::recovered);
    REQUIRE(!f.degraded());
}

TEST_CASE("db_failsafe: failures during the degraded drain push recovery out")
{
    /* After the trip severs everything, the queue's backlog keeps draining
     * against the broken sink and keeps producing failures; each one restarts
     * the quiet window, so recovery only follows genuine silence. */
    db_failsafe f(0, 3);
    REQUIRE(f.tick(1, 0, 3) == db_failsafe::event::tripped);
    REQUIRE(f.tick(2, 0, 2) == db_failsafe::event::none); // drain failure
    REQUIRE(f.tick(2, 0, 1) == db_failsafe::event::none); // quiet 1s
    REQUIRE(f.tick(3, 0, 1) == db_failsafe::event::none); // drain failure again
    REQUIRE(f.tick(3, 0, 0) == db_failsafe::event::none); // quiet 1s, drained
    REQUIRE(f.tick(3, 0, 0) == db_failsafe::event::none); // 2s
    REQUIRE(f.tick(3, 0, 0) == db_failsafe::event::none); // 3s
    REQUIRE(f.tick(3, 0, 0) == db_failsafe::event::recovered);
}

TEST_CASE("db_failsafe: a command drop while already degraded does not re-trip")
{
    db_failsafe f(0, 2);
    REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::tripped);
    /* Already degraded: the drop extends the quiet window but reports no new
     * transition (the caller has already severed and gated). */
    REQUIRE(f.tick(1, 1, 1) == db_failsafe::event::none);
    REQUIRE(f.degraded());
    REQUIRE(f.tick(1, 1, 0) == db_failsafe::event::none); // 1s quiet
    REQUIRE(f.tick(1, 1, 0) == db_failsafe::event::none); // 2s quiet
    REQUIRE(f.tick(1, 1, 0) == db_failsafe::event::recovered);
}

TEST_CASE("db_failsafe: a fresh incident after recovery trips again")
{
    /* The latched degraded state ends when the queue is drained and quiet;
     * readmitted traffic is the health probe. If the fault persists, the next
     * incident must latch again (one clean severance per incident, on the
     * incident timescale — not a per-tick flap). */
    db_failsafe f(2, 3);
    uint64_t failures = 0;
    REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::none);
    REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::none);
    REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::tripped);
    for (int i = 0; i < 3; i++)
    {
        REQUIRE(f.tick(failures, 0, 0) == db_failsafe::event::none);
    }
    REQUIRE(f.tick(failures, 0, 0) == db_failsafe::event::recovered);
    /* Same fault re-manifests once clients reconnect and write again. */
    REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::none);
    REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::none);
    REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::tripped);
    REQUIRE(f.degraded());
}

TEST_CASE("db_failsafe: telemetry-only drops from a real queue never trip the fail-safe")
{
    /* Drive the monitor from a real db_write_queue held at capacity by
     * protected command writes: incoming telemetry is rejected (the generic
     * dropped counter), which must not feed the command-integrity fail-safe —
     * telemetry stays loss-tolerant (todo/20). A subsequent genuine command
     * drop then trips it, pinning that the two counters reach the monitor on
     * distinct paths. */
    std::mutex gate_lock;
    std::condition_variable gate_cv;
    bool gate_open = false;
    std::atomic<int> entered{0};
    auto sink = [&](const fss::server::db_write_task & /*task*/) -> void {
        entered++;
        std::unique_lock<std::mutex> lock(gate_lock);
        gate_cv.wait(lock, [&]() -> bool { return gate_open; });
    };

    constexpr std::size_t depth = 3;
    fss::server::db_write_queue q(depth, sink);
    /* Park the worker on a first command so the queue fills predictably. */
    q.enqueue(fss::server::command_dispatch_write{1, 1});
    REQUIRE(fss_test::wait_for([&]() -> bool { return entered.load() >= 1; }));
    for (uint64_t i = 2; i <= depth + 1; i++)
    {
        q.enqueue(fss::server::command_dispatch_write{i, i});
    }
    REQUIRE(q.pending_count() == depth);

    /* Telemetry arrives with nothing to evict: rejected on the generic drop
     * path, no command dropped. */
    q.enqueue(fss::server::rtt_write{99, 0});
    REQUIRE(q.dropped_count() == 1);
    REQUIRE(q.command_dropped_count() == 0);

    db_failsafe f(5, 15);
    for (int i = 0; i < 20; i++)
    {
        REQUIRE(f.tick(q.write_failure_count(), q.command_dropped_count(), q.pending_count()) ==
                db_failsafe::event::none);
        REQUIRE(!f.degraded());
    }

    /* One more command overflows the all-command queue: the distinct
     * command-drop counter moves and the monitor trips on its next tick. */
    q.enqueue(fss::server::command_dispatch_write{50, 50});
    REQUIRE(q.command_dropped_count() == 1);
    REQUIRE(f.tick(q.write_failure_count(), q.command_dropped_count(), q.pending_count()) ==
            db_failsafe::event::tripped);
    REQUIRE(f.reason() == db_failsafe::trip_reason::command_drop);

    {
        std::scoped_lock guard(gate_lock);
        gate_open = true;
    }
    gate_cv.notify_all();
    q.stop();
}
