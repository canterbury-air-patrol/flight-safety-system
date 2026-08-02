#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <variant>

namespace flight_safety_system {
namespace server {

struct rtt_write {
    uint64_t asset_id;
    uint64_t rtt_ms;
};
struct position_write {
    uint64_t asset_id;
    /* NaN when the report carried no coordinates -- the wire no-fix sentinel.
     * The row is still written, with NULL geometry, so the operator can see
     * that the aircraft reported itself blind (todo/76). */
    double latitude;
    double longitude;
    uint32_t altitude;
    /* False when the coordinates are the autopilot's dead-reckoned estimate
     * rather than a GPS fix, or when there are no coordinates at all. */
    bool gps_fix_valid;
};
struct status_write {
    uint64_t asset_id;
    uint8_t bat_percent;
    uint32_t bat_mah_used;
    double bat_voltage;
};
struct search_status_write {
    uint64_t asset_id;
    uint64_t search_id;
    uint64_t completed;
    uint64_t total;
};
/* The dispatch id (per-connection message id) stamped on a command, recorded
 * against the command row so a later ack can be matched to it. */
struct command_dispatch_write {
    uint64_t command_dbid;
    uint64_t dispatch_id;
};
/* A command ack to store against the command row identified by its primary key
 * (todo/68). The acked id on the wire is the per-connection dispatch id, which is
 * unique to neither the asset nor the session; the acking session translates it to
 * the row id it dispatched before enqueueing, so what reaches the DB names exactly
 * one row and needs no scoping of its own. ack_state is the
 * fss_command_ack_outcome int, ack_reason the fss_command_ack_reason int. */
struct command_ack_write {
    uint64_t command_dbid;
    uint8_t ack_state;
    uint64_t ack_timestamp;
    uint8_t ack_reason;
};

using db_write_task = std::variant<rtt_write, position_write, status_write, search_status_write, command_dispatch_write,
                                   command_ack_write>;
using db_write_sink = std::function<void(const db_write_task &)>;
/* The DB fail-safe's health probe (todo/78), run on this queue's worker thread
 * because that thread already owns the write connection and already blocks on
 * it by design. Same failure contract as the sink: a throw is a failed probe.
 * The bool distinguishes the two non-throwing outcomes — true is positive
 * evidence that a write works, false is an inconclusive probe (nothing was
 * written because there was nothing to write against), which is deliberately
 * NOT counted as a success: the fail-safe readmits the fleet on a success. */
using db_probe_fn = std::function<bool()>;

/* Command dispatch/ack writes carry safety/audit state (the link between a sent
 * command and its ack) and must not be silently dropped under telemetry
 * pressure, unlike the loss-tolerant telemetry tasks (todo/20). */
inline auto is_command_task(const db_write_task &task) -> bool
{
    return std::holds_alternative<command_dispatch_write>(task) || std::holds_alternative<command_ack_write>(task);
}

/* C++17 overload helper for std::visit. */
template<class... Ts> struct overloaded : Ts... {
    using Ts::operator()...;
};
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

class db_write_queue {
private:
    std::size_t max_depth;
    db_write_sink sink;
    db_probe_fn probe;
    mutable std::mutex mtx{};
    std::condition_variable cv{};
    std::deque<db_write_task> q{};
    bool stopping{false};
    /* Set by requestProbe() under mtx, cleared by the worker when it takes the
     * probe: repeated requests coalesce, so at most one probe is ever
     * outstanding. Deliberately NOT a db_write_task: a queued probe would make
     * pending_count() non-zero, which the fail-safe's drain check reads as "not
     * drained" — recovery would become unreachable and the fleet would stay
     * severed forever, which is worse than the defect this closes. */
    std::atomic<bool> probe_requested{false};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> command_dropped{0};
    std::atomic<uint64_t> write_failures{0};
    std::atomic<uint64_t> probe_successes{0};
    std::atomic<uint64_t> probe_failures{0};
    std::atomic<uint64_t> probe_inconclusive{0};
    /* Last probe failure's message, for the caller's degraded-state log. */
    mutable std::mutex probe_error_mtx{};
    std::string probe_error{};
    std::thread worker{};

    void run();
    /* Run the probe and account for its outcome. Called on the worker thread
     * with mtx released. */
    void run_probe();
    /* Remove the oldest telemetry (non-command) task from the queue, if any.
     * Caller must hold mtx. Returns true if one was evicted. */
    auto evict_oldest_telemetry() -> bool;
public:
    /* t_probe is required, not defaulted: a server wired without one could
     * never leave the degraded state, so an unwired probe is a compile error
     * rather than a fleet that never comes back. */
    db_write_queue(std::size_t t_max_depth, db_write_sink t_sink, db_probe_fn t_probe);
    ~db_write_queue();
    db_write_queue(const db_write_queue &) = delete;
    db_write_queue(db_write_queue &&) = delete;
    auto operator=(const db_write_queue &) -> db_write_queue & = delete;
    auto operator=(db_write_queue &&) -> db_write_queue & = delete;

    void enqueue(db_write_task task);
    /* Ask the worker to run one health probe once the queue has drained. Never
     * touches the database itself, so the main loop can call it (todo/46's
     * no-synchronous-DB-work-on-the-main-loop contract). Repeated calls before
     * the worker wakes coalesce into one probe. */
    void requestProbe();
    void stop();
    auto dropped_count() const -> uint64_t;
    auto command_dropped_count() const -> uint64_t;
    auto write_failure_count() const -> uint64_t;
    /* Probes that wrote (and rolled back) a row: the fail-safe's evidence that
     * the write path is working. */
    auto probe_success_count() const -> uint64_t;
    auto probe_failure_count() const -> uint64_t;
    /* Probes that ran cleanly but proved nothing (no assets registered). */
    auto probe_inconclusive_count() const -> uint64_t;
    auto last_probe_error() const -> std::string;
    auto pending_count() const -> std::size_t;
};

} // namespace server
} // namespace flight_safety_system
