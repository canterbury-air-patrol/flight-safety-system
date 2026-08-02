#pragma once

#include "fss.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace flight_safety_system {
namespace server {

/* Unified DB fail-safe monitor for the main loop's per-second tick. The
 * rationale, and the two architectures rejected in favour of this one, are in
 * docs/decisions/34-45-47-db-failsafe-latch.md. Watches the db_write_queue's
 * failure counters and owns the decision to degrade:
 *
 * - A *sustained* write-failure incident: failures recurring within
 *   recovery_grace_secs of each other form one incident; the trip requires a
 *   new failure arriving once the incident is at least disconnect_age_secs
 *   old, i.e. failures genuinely spanning the window. A single transient
 *   failure ages out quietly and never trips (the tracker this replaced let a
 *   lone failure trip at the age threshold purely by staying "active" through
 *   the grace period, despite documenting the opposite).
 *
 * - Any command dispatch/ack drop: trips immediately on the first
 *   counter increase. A dropped command write is known-destroyed audit state
 *   (the command/ack link, or the aircraft's reported outcome), so there is
 *   no threshold to age through.
 *
 * A trip latches the degraded state: the caller severs every client
 * *and* gates new admissions (server_clients::setDegraded) so aircraft get one
 * clean comms-loss event instead of a disconnect/reconnect flap into the same
 * unhealthy server. Recovery requires the write queue fully drained, more than
 * recovery_grace_secs with no new failure, drop or probe failure, AND a
 * *successful health probe* observed on that very tick (todo/78).
 *
 * That last condition is the one field evidence forced. Drain-plus-quiet are
 * both conditions about the ABSENCE of failure, and the trip's own action —
 * disconnectAll() plus the admission gate — removes everything that could
 * produce a failure: no telemetry arrives, nothing is queued, nothing fails.
 * So they were satisfied by construction about recovery_grace_secs after every
 * trip, whatever the database was doing. Against a permanently dead database
 * (Path M m05: Postgres PANICked on a full disk and its crash recovery failed
 * too) that made a ~20 s flap cycle forever, each cycle taking a connected
 * aircraft out of and back into its comms-loss failsafe. The probe supplies the
 * positive evidence the quiet window cannot: a real write, on the connection
 * that failed, rolled back. It gates the quiet window rather than replacing it,
 * so with the caller's 1 s cadence and the default 15 s grace, recovery needs a
 * probe that keeps succeeding across the whole window, not one lucky moment: a
 * failing probe restarts the quiet window, and the recovering tick must itself
 * carry a fresh success. That freshness is the only thing that can see a probe
 * which HANGS rather than fails — against a partitioned or frozen database it
 * returns neither answer, so no counter moves and there is nothing else to
 * notice.
 *
 * Thresholds are measured against an injectable IClock, not against a count of
 * tick() calls (todo/70). The caller drives this from a loop whose period is
 * nominally one second but is really a sleep plus however long the loop body
 * took, and which a signal can cut short — so counting calls meant "5 seconds
 * of sustained failure before severing" stretched under load and compressed
 * under signal traffic. Load is correlated with the database being unwell,
 * which made the trip late in exactly the conditions it exists for. Measuring
 * real elapsed time also lets the tests assert the thresholds ("no trip at
 * 4.9 s, trip at 5.1 s") rather than the call count, which is the whole reason
 * every other timekeeping class in the tree takes a clock.
 *
 * Not thread-safe: main-loop tick only, like the tracker it replaces. */
class db_failsafe {
public:
    /* State transition reported by tick(); the caller performs the actual
     * severance/gating and logging, keeping I/O out of this class. */
    enum class event {
        none,
        tripped,
        recovered,
    };
    enum class trip_reason {
        none,
        write_failure,
        command_drop,
    };
    /* One tick's worth of the write queue's cumulative counters plus its
     * current backlog depth. A struct rather than five positional integers so
     * a caller cannot silently transpose two of them (and so clang-tidy's
     * easily-swappable-parameters check stays quiet as the set grows). */
    struct db_health_counters {
        uint64_t write_failures;
        uint64_t command_drops;
        /* Probes that wrote and rolled back a row. An *inconclusive* probe is
         * neither counted here nor as a failure: it is the absence of evidence
         * and must not readmit the fleet. */
        uint64_t probe_successes;
        uint64_t probe_failures;
        std::size_t pending_writes;
    };
private:
    static constexpr uint64_t ms_per_sec = 1000;
    uint64_t disconnect_age_ms;
    uint64_t recovery_grace_ms;
    std::shared_ptr<IClock> clock;
    uint64_t now_ms{0}; // the clock reading taken by the current tick
    uint64_t incident_start_ms{0};
    uint64_t last_event_ms{0}; // when a new failure or drop last arrived
    uint64_t last_write_failures{0};
    uint64_t last_command_drops{0};
    uint64_t last_probe_failures{0};
    uint64_t last_probe_successes{0};
    /* The probe-success count as of the tick that latched the degraded state.
     * Recovery requires the live count to differ from it, so successes banked
     * before the trip — from a previous degraded episode — cannot pay for this
     * one. Compared with !=, matching the counter idiom above. */
    uint64_t probe_successes_at_trip{0};
    /* An explicit flag rather than incident_start_ms == 0. A monotonic clock
     * that has just started, and every test clock, legitimately reads 0, so a
     * zero sentinel would silently discard an incident that began at the
     * origin. */
    bool incident_active{false};
    bool degraded_{false};
    trip_reason reason_{trip_reason::none};
public:
    /* Thresholds are given in seconds, as the config file states them, and held
     * in milliseconds because that is what IClock reports. A null clock falls
     * back to MonotonicClock, matching fss_client::setClock. */
    db_failsafe(uint64_t t_disconnect_age_secs, uint64_t t_recovery_grace_secs,
                std::shared_ptr<IClock> t_clock = nullptr)
        : disconnect_age_ms(t_disconnect_age_secs * ms_per_sec), recovery_grace_ms(t_recovery_grace_secs * ms_per_sec),
          clock(t_clock == nullptr ? std::make_shared<MonotonicClock>() : std::move(t_clock))
    {
    }
    /* Advance with the queue's current cumulative counters and backlog depth.
     * Counters are cumulative (never reset), so a change since the previous
     * tick is a new failure/drop since then. The caller is expected to tick
     * about once a second, but nothing here depends on that: every threshold
     * is measured against the clock, so a slow or interrupted tick changes when
     * a decision is observed, never when it is due. */
    auto tick(const db_health_counters &counters) -> event
    {
        this->now_ms = this->clock->now_ms();
        bool new_write_failure = counters.write_failures != this->last_write_failures;
        bool new_command_drop = counters.command_drops != this->last_command_drops;
        /* A probe failure resets the quiet window exactly like a write
         * failure — it is a demonstrated write failure, just one this server
         * provoked deliberately — but it is never a trip trigger: the probe
         * only runs while already degraded, and re-tripping a latched state
         * would only churn reason(). */
        bool new_probe_failure = counters.probe_failures != this->last_probe_failures;
        this->last_write_failures = counters.write_failures;
        this->last_command_drops = counters.command_drops;
        this->last_probe_failures = counters.probe_failures;
        if (new_write_failure || new_command_drop || new_probe_failure)
        {
            this->last_event_ms = this->now_ms;
        }
        if (this->degraded_)
        {
            /* The backlog must be gone before the quiet window means
             * anything: while tasks are still draining against a broken sink
             * they keep producing failures, and a nonempty-but-quiet queue
             * (e.g. a wedged sink) is not health either. */
            bool quiet = !new_write_failure && !new_command_drop && !new_probe_failure &&
                         (this->now_ms - this->last_event_ms) > this->recovery_grace_ms;
            /* The positive evidence (todo/78). Without it, drain and quiet are
             * both guaranteed by the severance this state performed, so the
             * degraded state ended on a timer no matter how dead the database
             * was.
             *
             * The evidence must be FRESH, not merely on record: recovery
             * happens on a tick that carries a probe success, not on a tick
             * that remembers one. A probe that HANGS — a partition, a failover,
             * a frozen host — returns neither answer, so no counter moves at
             * all: a failing probe holds the latch by restarting the quiet
             * window, but a hanging one can only be noticed by the absence of
             * fresh evidence. A "has succeeded at some point since the trip"
             * test lets a single early success pay for a recovery arbitrarily
             * later, into a database that has answered nothing since.
             *
             * Deliberately edge-triggered rather than "a success within the
             * last recovery_grace_ms", which sounds equivalent and is not: the
             * quiet window is measured from the trip, which is itself an event,
             * so recovery is first possible at trip + grace + one tick — and a
             * success arriving one tick after the trip is then almost exactly
             * grace old, i.e. still inside such a window. That formulation
             * therefore admits the very case it is meant to exclude, by a
             * margin of one tick. Requiring the success on the tick itself has
             * no constant in it and no boundary to land on.
             *
             * Cost on a healthy database: none. The caller requests a probe on
             * every degraded tick and the queue is empty while degraded, so
             * each tick observes the previous tick's success and recovery lands
             * on the same tick it always did. A database answering more slowly
             * than the tick period delays recovery by at most one probe round
             * trip — while it is answering that slowly, holding the gate up is
             * the safer error.
             *
             * The at-trip baseline is implied by freshness (a success observed
             * this tick is necessarily later than the trip's snapshot) and is
             * kept because the two state different invariants: this one says
             * "evidence now", that one says "evidence from THIS episode". A
             * later change to either must not silently inherit the other's
             * meaning.
             *
             * last_probe_successes is maintained here and at the two trip
             * sites rather than on every tick: probes are only ever requested
             * while degraded (wantsProbe()), so the counter cannot move in
             * between, and keeping the update beside its only reader is what
             * lets this flag live in the scope that uses it. */
            bool new_probe_success = counters.probe_successes != this->last_probe_successes;
            this->last_probe_successes = counters.probe_successes;
            bool probed_healthy = new_probe_success && counters.probe_successes != this->probe_successes_at_trip;
            if (counters.pending_writes == 0 && quiet && probed_healthy)
            {
                this->degraded_ = false;
                this->reason_ = trip_reason::none;
                this->incident_active = false;
                return event::recovered;
            }
            return event::none;
        }
        if (new_command_drop)
        {
            this->degraded_ = true;
            this->probe_successes_at_trip = counters.probe_successes;
            this->last_probe_successes = counters.probe_successes;
            this->reason_ = trip_reason::command_drop;
            /* No write-failure incident is implicated, so do not leave one
             * standing for incidentAgeSecs() to report against this trip. */
            this->incident_active = false;
            return event::tripped;
        }
        if (new_write_failure)
        {
            if (!this->incident_active)
            {
                this->incident_active = true;
                this->incident_start_ms = this->now_ms;
            }
            /* Not `else`: a disconnect_age_secs of 0 means "sever on any
             * failure", so the incident's first failure must satisfy the age
             * check on its own tick. */
            if ((this->now_ms - this->incident_start_ms) >= this->disconnect_age_ms)
            {
                this->degraded_ = true;
                this->probe_successes_at_trip = counters.probe_successes;
                this->last_probe_successes = counters.probe_successes;
                this->reason_ = trip_reason::write_failure;
                /* Deliberately left active, unlike the command-drop path: the
                 * caller logs incidentAgeSecs() with the trip, and nothing
                 * consults the incident while degraded — the branch above
                 * returns before reaching it. Recovery clears it. */
                return event::tripped;
            }
        }
        else if (this->incident_active && (this->now_ms - this->last_event_ms) > this->recovery_grace_ms)
        {
            this->incident_active = false;
        }
        return event::none;
    }
    [[nodiscard]] auto degraded() const -> bool { return this->degraded_; }
    /* Whether the caller should ask for a health probe this tick. True only
     * while degraded: in normal operation this is one bool read per second, no
     * enqueue, no atomic store and no database contact — the probe exists to
     * end the degraded state, and real traffic is evidence enough while it is
     * flowing. */
    [[nodiscard]] auto wantsProbe() const -> bool { return this->degraded_; }
    [[nodiscard]] auto reason() const -> trip_reason { return this->reason_; }
    /* How long the write-failure incident had been running as of the last
     * tick, in seconds (0 if none is active), for the caller's trip log. Read
     * against the tick's own clock reading rather than a fresh one, so the
     * number logged is the one the trip decision was made on. */
    [[nodiscard]] auto incidentAgeSecs() const -> uint64_t
    {
        return this->incident_active ? (this->now_ms - this->incident_start_ms) / ms_per_sec : 0;
    }
};

} // namespace server
} // namespace flight_safety_system
