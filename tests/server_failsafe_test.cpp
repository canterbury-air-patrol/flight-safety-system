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
using fss_test::FakeClock;

/* Unit tests for the unified DB fail-safe monitor (todo/45 + todo/47): the
 * write-failure incident semantics carried over from todo/34's tracker, the
 * immediate command-drop trip, and the latched degraded state with its
 * drain-plus-quiet-window recovery rule. */

namespace {

/* Drives db_failsafe on a clock the test owns (todo/70).
 *
 * Every case below was written when a tick() call *was* the unit of time, so
 * tick() here spends one second before each call and the cases read unchanged.
 * That is also what the caller does: the main loop ticks the fail-safe once a
 * second. What the clock adds is that the tests can now say how much time a
 * tick spent — see the threshold-edge and slow-tick cases at the end, which
 * were not expressible against a call counter.
 *
 * The three-argument tick()/tickAfter() also supply a *fresh probe success* on
 * every call (todo/78), because that is what the world those cases describe
 * looks like: the database answers writes. Before todo/78 recovery needed no
 * positive evidence at all, so leaving the probe counter still would silently
 * rewrite every recovery case here into a no-recovery case. The cases that are
 * about the probe itself use tickCounters() and drive all five counters
 * explicitly. */
class FailsafeHarness {
public:
    /* The back-off multiplier cap defaults to 1 — no back-off — so every case
     * written before todo/81 measures the configured grace and reads unchanged.
     * The cases that are about the back-off pass it explicitly. */
    FailsafeHarness(uint64_t t_disconnect_age_secs, uint64_t t_recovery_grace_secs, uint64_t t_recovery_backoff_max = 1)
        : f(t_disconnect_age_secs, t_recovery_grace_secs, t_recovery_backoff_max, clock)
    {
    }
    /* One nominal second of the caller's loop, against a database whose probe
     * is succeeding. */
    auto tick(uint64_t write_failures, uint64_t command_drops, std::size_t pending_writes) -> db_failsafe::event
    {
        return this->tickAfter(1000, write_failures, command_drops, pending_writes);
    }
    /* A tick that took `ms` to come round — a slow loop body, a signal-shortened
     * sleep, or a deliberate step onto one side of a threshold. */
    auto tickAfter(uint64_t ms, uint64_t write_failures, uint64_t command_drops, std::size_t pending_writes)
        -> db_failsafe::event
    {
        ++this->healthy_probe_successes;
        return this->tickCounters(
            ms, {write_failures, command_drops, this->healthy_probe_successes, this->probe_failures, pending_writes});
    }
    /* Full control of every counter the monitor reads. */
    auto tickCounters(uint64_t ms, const db_failsafe::db_health_counters &counters) -> db_failsafe::event
    {
        this->clock->advance(ms);
        return this->f.tick(counters);
    }
    /* Tick a degraded monitor against a database that is answering — healthy
     * probe, no new failures, empty queue — until it recovers, and report how
     * many nominal seconds that took. The bound is there so a regression that
     * stops recovery altogether fails the case instead of hanging the suite; it
     * is far above any window the cases below configure. */
    auto secondsToRecover(uint64_t write_failures, uint64_t command_drops) -> uint64_t
    {
        constexpr uint64_t bound = 600;
        for (uint64_t secs = 1; secs <= bound; secs++)
        {
            if (this->tick(write_failures, command_drops, 0) == db_failsafe::event::recovered)
            {
                return secs;
            }
        }
        return 0;
    }
    [[nodiscard]] auto degraded() const -> bool { return this->f.degraded(); }
    [[nodiscard]] auto recoveryGraceSecs() const -> uint64_t { return this->f.recoveryGraceSecs(); }
    [[nodiscard]] auto wantsProbe() const -> bool { return this->f.wantsProbe(); }
    [[nodiscard]] auto reason() const -> db_failsafe::trip_reason { return this->f.reason(); }
    [[nodiscard]] auto incidentAgeSecs() const -> uint64_t { return this->f.incidentAgeSecs(); }
    [[nodiscard]] auto degradedAgeSecs() const -> uint64_t { return this->f.degradedAgeSecs(); }
private:
    std::shared_ptr<FakeClock> clock{std::make_shared<FakeClock>()};
    db_failsafe f;
    uint64_t healthy_probe_successes{0};
    uint64_t probe_failures{0};
};

} // namespace

TEST_CASE("db_failsafe: a single transient write failure never trips")
{
    /* Pins the documented todo/34 intent ("A single transient failure never
     * trips this"). The pre-todo/47 tracker violated it whenever the recovery
     * grace exceeded the disconnect threshold — the default configuration —
     * because a lone failure kept the incident 'active' through the grace
     * period and the age check then fired with no further failure. */
    FailsafeHarness f(5, 15);
    REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::none);
    for (int i = 0; i < 30; i++)
    {
        REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::none);
        REQUIRE(!f.degraded());
    }
}

TEST_CASE("db_failsafe: sustained write failures trip once they span the age threshold")
{
    FailsafeHarness f(5, 15);
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
    FailsafeHarness f(4, 15);
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
    FailsafeHarness f(5, 3);
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
    FailsafeHarness f(0, 15);
    REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::tripped);
    REQUIRE(f.degraded());
    REQUIRE(f.reason() == db_failsafe::trip_reason::write_failure);
}

TEST_CASE("db_failsafe: any command drop trips immediately")
{
    /* todo/45: a dropped command dispatch/ack write is known-destroyed audit
     * state; there is no threshold to age through. */
    FailsafeHarness f(5, 15);
    REQUIRE(f.tick(0, 0, 0) == db_failsafe::event::none);
    REQUIRE(f.tick(0, 1, 0) == db_failsafe::event::tripped);
    REQUIRE(f.degraded());
    REQUIRE(f.reason() == db_failsafe::trip_reason::command_drop);
}

TEST_CASE("db_failsafe: no recovery while the write queue still holds a backlog")
{
    FailsafeHarness f(0, 2);
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
    FailsafeHarness f(0, 3);
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
    FailsafeHarness f(0, 3);
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
    FailsafeHarness f(0, 2);
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
    FailsafeHarness f(2, 3);
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
    fss::server::db_write_queue q(depth, sink, fss_test::healthy_probe);
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

    FailsafeHarness f(5, 15);
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

/* ── Threshold behaviour, which a call counter could not express ─────────── */

TEST_CASE("db_failsafe: the trip threshold is real time, not tick count (todo/70)")
{
    /* The question the class exists to answer and could not previously be
     * asked: does it fire when the configuration says it fires? The default is
     * 5 s of sustained failure, so a failure at 4.9 s must not trip and one at
     * 5.1 s must. */
    {
        FailsafeHarness f(5, 15);
        REQUIRE(f.tickAfter(1000, 1, 0, 0) == db_failsafe::event::none); // incident starts
        REQUIRE(f.tickAfter(3900, 2, 0, 0) == db_failsafe::event::none); // 4.9s old
        REQUIRE(!f.degraded());
    }
    {
        FailsafeHarness f(5, 15);
        REQUIRE(f.tickAfter(1000, 1, 0, 0) == db_failsafe::event::none);
        REQUIRE(f.tickAfter(5100, 2, 0, 0) == db_failsafe::event::tripped); // 5.1s old
        REQUIRE(f.reason() == db_failsafe::trip_reason::write_failure);
    }
}

TEST_CASE("db_failsafe: the recovery window is real time, not tick count (todo/70)")
{
    /* Same question for the other edge. The window is strictly greater than the
     * grace, so exactly 15.0 s of quiet is not yet recovery. */
    {
        FailsafeHarness f(0, 15);
        REQUIRE(f.tickAfter(1000, 1, 0, 0) == db_failsafe::event::tripped);
        REQUIRE(f.tickAfter(15000, 1, 0, 0) == db_failsafe::event::none); // exactly 15s
        REQUIRE(f.degraded());
    }
    {
        FailsafeHarness f(0, 15);
        REQUIRE(f.tickAfter(1000, 1, 0, 0) == db_failsafe::event::tripped);
        REQUIRE(f.tickAfter(15001, 1, 0, 0) == db_failsafe::event::recovered);
        REQUIRE(!f.degraded());
    }
}

TEST_CASE("db_failsafe: a slow tick does not move the threshold (todo/70)")
{
    /* The defect todo/70 filed. The caller's loop is a 100 ms sleep plus
     * however long the body took, and a signal can cut the sleep short — so
     * "5 seconds" used to mean "5 calls", stretching under load and
     * compressing under signal traffic. Load correlates with the database
     * being unwell, so the trip ran late in exactly the conditions it exists
     * for.
     *
     * Here one tick takes six seconds. Against a call counter the incident
     * would be one tick old and would not trip; against the clock it is six
     * seconds old and does. */
    FailsafeHarness f(5, 15);
    REQUIRE(f.tickAfter(1000, 1, 0, 0) == db_failsafe::event::none);
    REQUIRE(f.tickAfter(6000, 2, 0, 0) == db_failsafe::event::tripped);
    REQUIRE(f.reason() == db_failsafe::trip_reason::write_failure);
}

TEST_CASE("db_failsafe: a fast tick does not trip early (todo/70)")
{
    /* The other direction, which matters more: a SIGHUP returns the caller's
     * usleep early, so ticks can arrive far faster than once a second. Thirty
     * of them inside one second must not add up to a 5 s incident. */
    FailsafeHarness f(5, 15);
    uint64_t failures = 0;
    for (int i = 0; i < 30; i++)
    {
        REQUIRE(f.tickAfter(30, ++failures, 0, 0) == db_failsafe::event::none);
        REQUIRE(!f.degraded());
    }
}

TEST_CASE("db_failsafe: the trip reports how long the incident had been running (todo/70)")
{
    /* incidentAgeSecs() is documented as existing for the caller's trip log and
     * had no caller; server.cpp now logs it, so an operator can tell a
     * just-over-threshold trip from one that had been failing for a minute.
     * The write-failure path leaves the incident standing for exactly this
     * read; the command-drop path has no incident to report. */
    {
        FailsafeHarness f(5, 15);
        REQUIRE(f.tickAfter(1000, 1, 0, 0) == db_failsafe::event::none);
        REQUIRE(f.tickAfter(42000, 2, 0, 0) == db_failsafe::event::tripped);
        REQUIRE(f.incidentAgeSecs() == 42);
    }
    {
        FailsafeHarness f(5, 15);
        REQUIRE(f.tickAfter(1000, 0, 1, 0) == db_failsafe::event::tripped);
        REQUIRE(f.reason() == db_failsafe::trip_reason::command_drop);
        REQUIRE(f.incidentAgeSecs() == 0);
    }
}

TEST_CASE("db_failsafe: an incident starting at clock zero is not discarded (todo/70)")
{
    /* Regression guard for the sentinel this conversion removed. The old code
     * used incident_start == 0 to mean "no incident", which a clock reading
     * zero — every FakeClock, and a monotonic clock just after boot —
     * indistinguishably satisfies. With the sentinel, this incident would
     * silently restart on every failure and never age enough to trip. */
    FailsafeHarness f(2, 15);
    REQUIRE(f.tickAfter(0, 1, 0, 0) == db_failsafe::event::none); // incident starts at t=0
    REQUIRE(f.tickAfter(1000, 2, 0, 0) == db_failsafe::event::none);
    REQUIRE(f.tickAfter(1000, 3, 0, 0) == db_failsafe::event::tripped); // 2s old
    REQUIRE(f.reason() == db_failsafe::trip_reason::write_failure);
}

/* ── Recovery needs positive evidence, not silence (todo/78) ─────────────── */

TEST_CASE("db_failsafe: silence alone never recovers the degraded state (todo/78)")
{
    /* The m05 field regression, distilled. A 200 MB Postgres data directory
     * filled until the instance PANICked and its crash recovery failed too, so
     * the database was permanently gone — and the server announced "DB fail-safe
     * recovered: write queue drained and quiet for 15s" sixteen seconds after
     * the trip, readmitting the aircraft into a database that could not record
     * a single thing about it.
     *
     * Both of the old conditions are about the ABSENCE of failure, and the
     * trip's own action removes everything that could produce one: every client
     * is severed and new ones refused, so nothing is queued and nothing fails.
     * Drained and quiet were therefore satisfied by construction. Here they are
     * satisfied for ten times the grace window with the probe never once
     * succeeding, and the state must hold. */
    FailsafeHarness f(0, 15);
    REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 0}) == db_failsafe::event::tripped);
    for (int i = 0; i < 150; i++)
    {
        REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 0}) == db_failsafe::event::none);
        REQUIRE(f.degraded());
        REQUIRE(f.reason() == db_failsafe::trip_reason::write_failure);
    }
}

TEST_CASE("db_failsafe: a probe success after the quiet window recovers (todo/78)")
{
    FailsafeHarness f(0, 3);
    REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 0}) == db_failsafe::event::tripped);
    /* Drained and quiet, but no evidence: the window elapsing is not enough. */
    REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 0}) == db_failsafe::event::none); // 1s
    REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 0}) == db_failsafe::event::none); // 2s
    REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 0}) == db_failsafe::event::none); // 3s
    REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 0}) == db_failsafe::event::none); // 4s, quiet satisfied
    REQUIRE(f.degraded());
    /* The probe wrote a row (and rolled it back): the write path works. */
    REQUIRE(f.tickCounters(1000, {1, 0, 1, 0, 0}) == db_failsafe::event::recovered);
    REQUIRE(!f.degraded());
    REQUIRE(f.reason() == db_failsafe::trip_reason::none);
}

TEST_CASE("db_failsafe: a probe success does not shortcut the quiet window (todo/78)")
{
    /* The probe gates the existing window, it does not replace it. A database
     * that answers one probe in the middle of a failure burst has not shown it
     * can carry the fleet's writes. */
    FailsafeHarness f(0, 3);
    REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 0}) == db_failsafe::event::tripped);
    REQUIRE(f.tickCounters(1000, {1, 0, 1, 0, 0}) == db_failsafe::event::none); // 1s quiet
    REQUIRE(f.tickCounters(1000, {1, 0, 2, 0, 0}) == db_failsafe::event::none); // 2s
    REQUIRE(f.tickCounters(1000, {1, 0, 3, 0, 0}) == db_failsafe::event::none); // 3s
    REQUIRE(f.degraded());
    REQUIRE(f.tickCounters(1000, {1, 0, 4, 0, 0}) == db_failsafe::event::recovered); // 4s > grace
}

TEST_CASE("db_failsafe: a probe failure restarts the quiet window (todo/78)")
{
    FailsafeHarness f(0, 3);
    REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 0}) == db_failsafe::event::tripped);
    REQUIRE(f.tickCounters(1000, {1, 0, 1, 0, 0}) == db_failsafe::event::none); // a success banked
    /* A probe failure is a demonstrated write failure, so it resets the window
     * exactly as a real one does — recovery is deferred a full grace from here,
     * not from the trip. */
    REQUIRE(f.tickCounters(1000, {1, 0, 1, 1, 0}) == db_failsafe::event::none);
    for (int i = 0; i < 3; i++)
    {
        REQUIRE(f.tickCounters(1000, {1, 0, 2, 1, 0}) == db_failsafe::event::none);
        REQUIRE(f.degraded());
    }
    REQUIRE(f.tickCounters(1000, {1, 0, 3, 1, 0}) == db_failsafe::event::recovered);
}

TEST_CASE("db_failsafe: a permanently failing probe never recovers (todo/78)")
{
    /* Five simulated minutes against a database that is simply gone. Zero
     * recovered events — which is what the aircraft in m05 needed: one latched
     * comms-loss event, not one every twenty seconds. */
    FailsafeHarness f(0, 15);
    REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 0}) == db_failsafe::event::tripped);
    uint64_t probe_failures = 0;
    for (int i = 0; i < 300; i++)
    {
        REQUIRE(f.tickCounters(1000, {1, 0, 0, ++probe_failures, 0}) == db_failsafe::event::none);
        REQUIRE(f.degraded());
    }
}

TEST_CASE("db_failsafe: wantsProbe is true exactly while degraded (todo/78)")
{
    /* The cadence rule. Healthy operation must not pay for the probe at all:
     * the caller's per-second block reads this one bool and does nothing else. */
    FailsafeHarness f(0, 3);
    REQUIRE(!f.wantsProbe());
    REQUIRE(f.tickCounters(1000, {0, 0, 0, 0, 0}) == db_failsafe::event::none);
    REQUIRE(!f.wantsProbe());
    REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 0}) == db_failsafe::event::tripped);
    for (int i = 0; i < 4; i++)
    {
        REQUIRE(f.wantsProbe());
        REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 0}) == db_failsafe::event::none);
    }
    REQUIRE(f.wantsProbe());
    REQUIRE(f.tickCounters(1000, {1, 0, 1, 0, 0}) == db_failsafe::event::recovered);
    REQUIRE(!f.wantsProbe());
}

TEST_CASE("db_failsafe: probe successes banked before the trip do not count (todo/78)")
{
    /* The baseline is taken from the tick that latches, so a previous degraded
     * episode's evidence cannot pay for this one — otherwise a server that
     * recovered once would recover instantly forever after. */
    FailsafeHarness f(0, 3);
    REQUIRE(f.tickCounters(1000, {0, 0, 7, 0, 0}) == db_failsafe::event::none);
    REQUIRE(f.tickCounters(1000, {1, 0, 7, 0, 0}) == db_failsafe::event::tripped);
    for (int i = 0; i < 10; i++)
    {
        REQUIRE(f.tickCounters(1000, {1, 0, 7, 0, 0}) == db_failsafe::event::none);
        REQUIRE(f.degraded());
    }
    REQUIRE(f.tickCounters(1000, {1, 0, 8, 0, 0}) == db_failsafe::event::recovered);
}

TEST_CASE("db_failsafe: a probe success does not recover while the queue holds a backlog (todo/78)")
{
    FailsafeHarness f(0, 2);
    REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 5}) == db_failsafe::event::tripped);
    uint64_t successes = 0;
    for (int i = 0; i < 10; i++)
    {
        REQUIRE(f.tickCounters(1000, {1, 0, ++successes, 0, 5}) == db_failsafe::event::none);
        REQUIRE(f.degraded());
    }
    REQUIRE(f.tickCounters(1000, {1, 0, ++successes, 0, 0}) == db_failsafe::event::recovered);
}

TEST_CASE("db_failsafe: a probe failure while degraded never re-trips (todo/78)")
{
    /* A probe failure feeds the quiet window and nothing else: the state is
     * already latched, and re-reporting a trip would churn the caller's
     * severance path and rewrite reason(). Checked with disconnect_age_secs 0,
     * the setting that trips on any single write failure. */
    FailsafeHarness f(0, 3);
    REQUIRE(f.tickCounters(1000, {0, 1, 0, 0, 0}) == db_failsafe::event::tripped);
    REQUIRE(f.reason() == db_failsafe::trip_reason::command_drop);
    uint64_t probe_failures = 0;
    for (int i = 0; i < 10; i++)
    {
        REQUIRE(f.tickCounters(1000, {0, 1, 0, ++probe_failures, 0}) == db_failsafe::event::none);
        REQUIRE(f.degraded());
        REQUIRE(f.reason() == db_failsafe::trip_reason::command_drop);
    }
}

TEST_CASE("db_failsafe: a command-drop trip also requires a probe success (todo/78)")
{
    /* Both triggers share the one latch, so both share the one recovery rule.
     * A dropped command write is known-destroyed audit state; readmitting the
     * fleet on silence would be no better founded here than for a write
     * failure. */
    FailsafeHarness f(5, 3);
    REQUIRE(f.tickCounters(1000, {0, 1, 0, 0, 0}) == db_failsafe::event::tripped);
    for (int i = 0; i < 10; i++)
    {
        REQUIRE(f.tickCounters(1000, {0, 1, 0, 0, 0}) == db_failsafe::event::none);
        REQUIRE(f.degraded());
    }
    REQUIRE(f.tickCounters(1000, {0, 1, 1, 0, 0}) == db_failsafe::event::recovered);
    REQUIRE(f.reason() == db_failsafe::trip_reason::none);
}

TEST_CASE("db_failsafe: a stale probe success does not recover a hung database (todo/78)")
{
    /* The gap between "has ever succeeded since the trip" and "is succeeding".
     * A *failing* probe holds the latch by restarting the quiet window. A
     * probe that HANGS — a network partition, a failover, a frozen host —
     * returns neither answer: no success, no failure, no counter movement, and
     * requestProbe() coalesces so nothing accumulates behind it either. The only
     * observable is that the last answer keeps getting older.
     *
     * Here one probe succeeds a second after the trip and the database then
     * stops answering entirely. With an evidence test that had no recency
     * requirement, that single success would still be paying for recovery ten
     * grace windows later. */
    FailsafeHarness f(0, 15);
    REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 0}) == db_failsafe::event::tripped);
    REQUIRE(f.tickCounters(1000, {1, 0, 1, 0, 0}) == db_failsafe::event::none); // the last answer ever given
    for (int i = 0; i < 150; i++)
    {
        REQUIRE(f.tickCounters(1000, {1, 0, 1, 0, 0}) == db_failsafe::event::none);
        REQUIRE(f.degraded());
        REQUIRE(f.reason() == db_failsafe::trip_reason::write_failure);
    }
}

TEST_CASE("db_failsafe: a probe that resumes answering recovers on that tick (todo/78)")
{
    /* The other half of the hung-database rule: holding the latch on missing
     * evidence must not become holding it forever. Once the probe starts
     * answering again the state ends on the tick that carries the answer —
     * not before it (no credit for the silence) and not later (no extra
     * penalty box). */
    FailsafeHarness f(0, 15);
    REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 0}) == db_failsafe::event::tripped);
    REQUIRE(f.tickCounters(1000, {1, 0, 1, 0, 0}) == db_failsafe::event::none);
    /* Sixty seconds of a hung probe: quiet has long since elapsed. */
    for (int i = 0; i < 60; i++)
    {
        REQUIRE(f.tickCounters(1000, {1, 0, 1, 0, 0}) == db_failsafe::event::none);
    }
    REQUIRE(f.degraded());
    REQUIRE(f.tickCounters(1000, {1, 0, 2, 0, 0}) == db_failsafe::event::recovered);
}

TEST_CASE("db_failsafe: an in-window but stale success is not evidence (todo/78)")
{
    /* Why the freshness test is edge-triggered rather than "a success within
     * the last recovery_grace_ms". Those sound equivalent; they are not, and
     * the difference is exactly the case this rule exists for.
     *
     * The quiet window runs from the trip, which is itself an event, so
     * recovery is first possible one tick after trip + grace. A success
     * arriving one tick AFTER the trip is, at that moment, almost exactly
     * grace old — inside a "within the last grace" window by a whole tick. So
     * that formulation would readmit the fleet here, having had one answer at
     * t=2s and none since. Written against the clock so the arithmetic is
     * visible: trip at 1s, sole success at 2s, quiet satisfied from 16.001s. */
    FailsafeHarness f(0, 15);
    REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 0}) == db_failsafe::event::tripped);
    REQUIRE(f.tickCounters(1000, {1, 0, 1, 0, 0}) == db_failsafe::event::none);
    REQUIRE(f.tickCounters(14001, {1, 0, 1, 0, 0}) == db_failsafe::event::none); // t=16.001s, quiet
    REQUIRE(f.degraded());
    REQUIRE(f.tickCounters(999, {1, 0, 1, 0, 0}) == db_failsafe::event::none); // t=17s
    REQUIRE(f.degraded());
}

TEST_CASE("db_failsafe: the freshness rule does not delay a normal recovery (todo/78)")
{
    /* The freshness test must be free on a healthy database: the caller
     * requests a probe on every degraded tick and the write queue is empty
     * while degraded, so every tick carries the previous tick's success.
     * Recovery lands on exactly the tick it landed on before the rule existed —
     * compare with "recovery requires the full quiet window after the last
     * failure" above, which is this same sequence driven through the harness's
     * healthy-probe default. */
    FailsafeHarness f(0, 3);
    uint64_t successes = 0;
    REQUIRE(f.tickCounters(1000, {1, 0, successes, 0, 0}) == db_failsafe::event::tripped);
    REQUIRE(f.tickCounters(1000, {1, 0, ++successes, 0, 0}) == db_failsafe::event::none); // 1s quiet
    REQUIRE(f.tickCounters(1000, {1, 0, ++successes, 0, 0}) == db_failsafe::event::none); // 2s
    REQUIRE(f.tickCounters(1000, {1, 0, ++successes, 0, 0}) == db_failsafe::event::none); // 3s
    REQUIRE(f.tickCounters(1000, {1, 0, ++successes, 0, 0}) == db_failsafe::event::recovered);
    REQUIRE(!f.degraded());
}

TEST_CASE("db_failsafe: a probe slower than the tick delays recovery by at most one round trip (todo/78)")
{
    /* A database that answers, but slowly (a loaded instance, a long lock
     * wait): probes complete every ~5s while the caller ticks every second, so
     * most ticks carry no fresh success. Recovery must wait for one — and must
     * not wait longer than the next one. The bound the state machine offers is
     * therefore "quiet window plus at most one probe round trip", which is
     * worth stating because it is the only case where this rule costs
     * anything. */
    FailsafeHarness f(0, 15);
    uint64_t successes = 0;
    REQUIRE(f.tickCounters(1000, {1, 0, successes, 0, 0}) == db_failsafe::event::tripped); // t=1s
    /* Ticks 1..15 (t=2s..16s), an answer landing on every fifth. */
    for (int i = 1; i <= 15; i++)
    {
        if (i % 5 == 0)
        {
            ++successes;
        }
        REQUIRE(f.tickCounters(1000, {1, 0, successes, 0, 0}) == db_failsafe::event::none);
    }
    /* t=17s: quiet is satisfied (16s since the trip) but the last answer landed
     * at t=16s and this tick carries nothing new. */
    REQUIRE(f.tickCounters(1000, {1, 0, successes, 0, 0}) == db_failsafe::event::none);
    REQUIRE(f.degraded());
    REQUIRE(f.tickCounters(1000, {1, 0, successes, 0, 0}) == db_failsafe::event::none); // t=18s
    REQUIRE(f.tickCounters(1000, {1, 0, successes, 0, 0}) == db_failsafe::event::none); // t=19s
    /* t=20s, the next completed probe: recovery, one round trip after quiet. */
    REQUIRE(f.tickCounters(1000, {1, 0, ++successes, 0, 0}) == db_failsafe::event::recovered);
}

TEST_CASE("db_failsafe: degradedAgeSecs measures the severance, not the incident (todo/78)")
{
    /* The periodic still-degraded log reports this, and it is deliberately not
     * incidentAgeSecs(): an incident is a run of write failures, which the
     * command-drop path does not have at all, while this measures how long
     * sessions have actually been refused. Read against the tick's own clock,
     * for the same reason incidentAgeSecs() is — the number logged is the one
     * the decision was made on. */
    SECTION("zero before a trip, and again after recovery")
    {
        FailsafeHarness f(0, 15);
        REQUIRE(f.degradedAgeSecs() == 0);
        REQUIRE(f.tick(0, 0, 0) == db_failsafe::event::none);
        REQUIRE(f.degradedAgeSecs() == 0);
        REQUIRE(f.tick(1, 0, 0) == db_failsafe::event::tripped);
        for (int i = 0; i < 16; i++)
        {
            f.tick(1, 0, 0);
        }
        REQUIRE_FALSE(f.degraded());
        REQUIRE(f.degradedAgeSecs() == 0);
    }
    SECTION("counts from the trip while the state is latched")
    {
        FailsafeHarness f(0, 15);
        REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 0}) == db_failsafe::event::tripped);
        REQUIRE(f.degradedAgeSecs() == 0);
        /* A hung probe: no counter moves, so the latch holds and the age runs. */
        for (int i = 1; i <= 60; i++)
        {
            REQUIRE(f.tickCounters(1000, {1, 0, 0, 0, 0}) == db_failsafe::event::none);
            REQUIRE(f.degradedAgeSecs() == static_cast<uint64_t>(i));
        }
    }
    SECTION("a command-drop trip has no incident but still has a degraded age")
    {
        FailsafeHarness f(5, 15);
        REQUIRE(f.tickCounters(1000, {0, 1, 0, 0, 0}) == db_failsafe::event::tripped);
        REQUIRE(f.reason() == db_failsafe::trip_reason::command_drop);
        REQUIRE(f.incidentAgeSecs() == 0);
        REQUIRE(f.tickCounters(1000, {0, 1, 0, 0, 0}) == db_failsafe::event::none);
        REQUIRE(f.degradedAgeSecs() == 1);
    }
}

TEST_CASE("db_failsafe: a repeated trip holds the gate proportionally longer (todo/81)")
{
    /* The intermittent-fault case todo/78 left open. Every readmission below is
     * backed by evidence that was true when it was taken — a fresh probe
     * success after a full quiet window — and the fault still comes back, so
     * without the back-off the aircraft would keep cycling in and out of its
     * comms-loss state at the incident timescale. What grows is the cost of the
     * repetition; the first trip is untouched. */
    FailsafeHarness f(0, 3, 4);
    uint64_t failures = 0;
    /* First trip of the run: the configured grace, so recovery lands on the
     * first tick strictly past it. */
    REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::tripped);
    REQUIRE(f.recoveryGraceSecs() == 3);
    REQUIRE(f.secondsToRecover(failures, 0) == 4);
    /* The fault returns as soon as the readmitted fleet writes again. */
    REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::tripped);
    REQUIRE(f.recoveryGraceSecs() == 6);
    REQUIRE(f.secondsToRecover(failures, 0) == 7);
    REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::tripped);
    REQUIRE(f.recoveryGraceSecs() == 9);
    REQUIRE(f.secondsToRecover(failures, 0) == 10);
}

TEST_CASE("db_failsafe: the recovery back-off is capped (todo/81)")
{
    /* Unbounded growth would eventually refuse a fleet against a database that
     * had genuinely recovered — the fail-safe's own failure mode, arrived at
     * from the cautious side. */
    FailsafeHarness f(0, 3, 2);
    uint64_t failures = 0;
    REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::tripped);
    REQUIRE(f.secondsToRecover(failures, 0) == 4);
    REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::tripped);
    REQUIRE(f.secondsToRecover(failures, 0) == 7); // 2x, the cap
    for (int i = 0; i < 3; i++)
    {
        REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::tripped);
        REQUIRE(f.recoveryGraceSecs() == 6);
        REQUIRE(f.secondsToRecover(failures, 0) == 7);
    }
}

TEST_CASE("db_failsafe: a cap of 1 disables the back-off (todo/81)")
{
    /* The escape hatch, and the pre-todo/81 behaviour: a deployment that would
     * rather readmit early every time can say so. A cap of 0 is read as 1 by
     * the constructor rather than as a grace of zero, which would readmit on
     * the first quiet tick. */
    for (uint64_t cap : {uint64_t{1}, uint64_t{0}})
    {
        FailsafeHarness f(0, 3, cap);
        uint64_t failures = 0;
        for (int i = 0; i < 4; i++)
        {
            REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::tripped);
            REQUIRE(f.recoveryGraceSecs() == 3);
            REQUIRE(f.secondsToRecover(failures, 0) == 4);
        }
    }
}

TEST_CASE("db_failsafe: a trip after a quiet hour starts a fresh run (todo/81)")
{
    /* The back-off is about a fault that keeps coming back, not a tally kept
     * for the life of the process: an unrelated incident tomorrow must cost the
     * fleet no more than today's first one did. */
    FailsafeHarness f(0, 3, 4);
    uint64_t failures = 0;
    REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::tripped);
    REQUIRE(f.secondsToRecover(failures, 0) == 4);
    REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::tripped);
    REQUIRE(f.recoveryGraceSecs() == 6);
    REQUIRE(f.secondsToRecover(failures, 0) == 7);
    /* Two hours of a healthy database, then a new fault. */
    constexpr uint64_t two_hours_ms = 2 * 60 * 60 * 1000;
    REQUIRE(f.tickAfter(two_hours_ms, failures, 0, 0) == db_failsafe::event::none);
    REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::tripped);
    REQUIRE(f.recoveryGraceSecs() == 3);
    REQUIRE(f.secondsToRecover(failures, 0) == 4);
}

TEST_CASE("db_failsafe: the back-off applies to a command-drop trip too (todo/81)")
{
    /* Both trip triggers latch the same degraded state and both are readmitted
     * by the same window, so a database dropping command writes intermittently
     * flaps exactly as a write-failing one does. */
    FailsafeHarness f(5, 3, 4);
    uint64_t drops = 0;
    REQUIRE(f.tick(0, ++drops, 0) == db_failsafe::event::tripped);
    REQUIRE(f.reason() == db_failsafe::trip_reason::command_drop);
    REQUIRE(f.secondsToRecover(0, drops) == 4);
    REQUIRE(f.tick(0, ++drops, 0) == db_failsafe::event::tripped);
    REQUIRE(f.recoveryGraceSecs() == 6);
    REQUIRE(f.secondsToRecover(0, drops) == 7);
}

TEST_CASE("db_failsafe: the back-off does not stretch the trip threshold (todo/81)")
{
    /* recovery_grace_secs has a second role outside a degraded episode: how
     * long failures may pause and still chain into one incident. Backing that
     * off too would make each successive trip *later* as well as longer — the
     * fleet would keep writing into a database already known to be failing. */
    FailsafeHarness f(5, 3, 4);
    uint64_t failures = 0;
    for (int age = 0; age < 5; age++)
    {
        REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::none);
    }
    REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::tripped);
    REQUIRE(f.secondsToRecover(failures, 0) == 4);
    /* Second incident, same shape: the trip still needs failures spanning
     * exactly disconnect_age_secs, and the incident-chaining window is still
     * the configured 3s — only the hold that follows is doubled. */
    for (int age = 0; age < 5; age++)
    {
        REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::none);
    }
    REQUIRE(f.tick(++failures, 0, 0) == db_failsafe::event::tripped);
    REQUIRE(f.secondsToRecover(failures, 0) == 7);
}
