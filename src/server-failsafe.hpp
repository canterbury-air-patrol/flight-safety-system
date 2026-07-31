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
 * unhealthy server. Recovery requires the write queue fully drained AND more
 * than recovery_grace_secs with no new failure or drop; readmitted traffic is
 * then the health probe — if the fault persists, the next incident latches
 * again rather than flapping on the tick period.
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
    auto tick(uint64_t write_failures, uint64_t command_drops, std::size_t pending_writes) -> event
    {
        this->now_ms = this->clock->now_ms();
        bool new_write_failure = write_failures != this->last_write_failures;
        bool new_command_drop = command_drops != this->last_command_drops;
        this->last_write_failures = write_failures;
        this->last_command_drops = command_drops;
        if (new_write_failure || new_command_drop)
        {
            this->last_event_ms = this->now_ms;
        }
        if (this->degraded_)
        {
            /* The backlog must be gone before the quiet window means
             * anything: while tasks are still draining against a broken sink
             * they keep producing failures, and a nonempty-but-quiet queue
             * (e.g. a wedged sink) is not health either. */
            bool quiet = !new_write_failure && !new_command_drop &&
                         (this->now_ms - this->last_event_ms) > this->recovery_grace_ms;
            if (pending_writes == 0 && quiet)
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
