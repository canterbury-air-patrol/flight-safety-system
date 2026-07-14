#pragma once

#include <cstddef>
#include <cstdint>

namespace flight_safety_system {
namespace server {

/* Unified DB fail-safe monitor for the main loop's per-second tick (todo/34,
 * todo/45, todo/47). Watches the db_write_queue's failure counters and owns
 * the decision to degrade:
 *
 * - A *sustained* write-failure incident (todo/34): failures recurring within
 *   recovery_grace_secs of each other form one incident; the trip requires a
 *   new failure arriving once the incident is at least disconnect_age_secs
 *   old, i.e. failures genuinely spanning the window. A single transient
 *   failure ages out quietly and never trips (the pre-todo/47 tracker let a
 *   lone failure trip at the age threshold purely by staying "active" through
 *   the grace period, despite documenting the opposite).
 *
 * - Any command dispatch/ack drop (todo/45): trips immediately on the first
 *   counter increase. A dropped command write is known-destroyed audit state
 *   (the command/ack link, or the aircraft's reported outcome), so there is
 *   no threshold to age through.
 *
 * A trip latches the degraded state (todo/47): the caller severs every client
 * *and* gates new admissions (server_clients::setDegraded) so aircraft get one
 * clean comms-loss event instead of a disconnect/reconnect flap into the same
 * unhealthy server. Recovery requires the write queue fully drained AND more
 * than recovery_grace_secs with no new failure or drop; readmitted traffic is
 * then the health probe — if the fault persists, the next incident latches
 * again rather than flapping on the tick period.
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
    uint64_t disconnect_age_secs;
    uint64_t recovery_grace_secs;
    uint64_t elapsed_secs{0};
    uint64_t incident_start_secs{0}; // 0 = no active write-failure incident
    uint64_t last_event_secs{0};     // last tick a new failure or drop arrived
    uint64_t last_write_failures{0};
    uint64_t last_command_drops{0};
    bool degraded_{false};
    trip_reason reason_{trip_reason::none};
public:
    db_failsafe(uint64_t t_disconnect_age_secs, uint64_t t_recovery_grace_secs)
        : disconnect_age_secs(t_disconnect_age_secs), recovery_grace_secs(t_recovery_grace_secs)
    {
    }
    /* Advance one second with the queue's current cumulative counters and
     * backlog depth. Counters are cumulative (never reset), so a change since
     * the previous tick is a new failure/drop in the last second. */
    auto tick(uint64_t write_failures, uint64_t command_drops, std::size_t pending_writes) -> event
    {
        this->elapsed_secs++;
        bool new_write_failure = write_failures != this->last_write_failures;
        bool new_command_drop = command_drops != this->last_command_drops;
        this->last_write_failures = write_failures;
        this->last_command_drops = command_drops;
        if (new_write_failure || new_command_drop)
        {
            this->last_event_secs = this->elapsed_secs;
        }
        if (this->degraded_)
        {
            /* The backlog must be gone before the quiet window means
             * anything: while tasks are still draining against a broken sink
             * they keep producing failures, and a nonempty-but-quiet queue
             * (e.g. a wedged sink) is not health either. */
            bool quiet = !new_write_failure && !new_command_drop &&
                         (this->elapsed_secs - this->last_event_secs) > this->recovery_grace_secs;
            if (pending_writes == 0 && quiet)
            {
                this->degraded_ = false;
                this->reason_ = trip_reason::none;
                this->incident_start_secs = 0;
                return event::recovered;
            }
            return event::none;
        }
        if (new_command_drop)
        {
            this->degraded_ = true;
            this->reason_ = trip_reason::command_drop;
            this->incident_start_secs = 0;
            return event::tripped;
        }
        if (new_write_failure)
        {
            if (this->incident_start_secs == 0)
            {
                this->incident_start_secs = this->elapsed_secs;
            }
            /* Not `else`: a disconnect_age_secs of 0 means "sever on any
             * failure", so the incident's first failure must satisfy the age
             * check on its own tick. */
            if ((this->elapsed_secs - this->incident_start_secs) >= this->disconnect_age_secs)
            {
                this->degraded_ = true;
                this->reason_ = trip_reason::write_failure;
                this->incident_start_secs = 0;
                return event::tripped;
            }
        }
        else if (this->incident_start_secs != 0 &&
                 (this->elapsed_secs - this->last_event_secs) > this->recovery_grace_secs)
        {
            this->incident_start_secs = 0;
        }
        return event::none;
    }
    [[nodiscard]] auto degraded() const -> bool { return this->degraded_; }
    [[nodiscard]] auto reason() const -> trip_reason { return this->reason_; }
    /* The active write-failure incident's age in seconds (0 if none), for the
     * caller's trip log. */
    [[nodiscard]] auto incidentAgeSecs() const -> uint64_t
    {
        return this->incident_start_secs == 0 ? 0 : this->elapsed_secs - this->incident_start_secs;
    }
};

} // namespace server
} // namespace flight_safety_system
