#include "db-write-queue.hpp"
#include "fss-log.hpp"

#include <algorithm>
#include <exception>
#include <system_error>
#include <thread>
#include <utility>

namespace flight_safety_system::server {

db_write_queue::db_write_queue(std::size_t t_max_depth, db_write_sink t_sink, db_probe_fn t_probe)
    : max_depth(std::max<std::size_t>(1, t_max_depth)), sink(std::move(t_sink)), probe(std::move(t_probe))
{
    this->worker = std::thread(&db_write_queue::run, this);
}

db_write_queue::~db_write_queue()
{
    this->stop();
}

auto db_write_queue::evict_oldest_telemetry() -> bool
{
    auto it = std::find_if(this->q.begin(), this->q.end(),
                           [](const db_write_task &t) -> bool { return !is_command_task(t); });
    if (it == this->q.end())
    {
        return false;
    }
    this->q.erase(it);
    return true;
}

void db_write_queue::enqueue(db_write_task task)
{
    bool dropped_telemetry = false;
    bool dropped_command = false;
    uint64_t total_telemetry_dropped = 0;
    uint64_t total_command_dropped = 0;
    bool enqueued = true;
    {
        std::scoped_lock guard(this->mtx);
        if (this->stopping)
        {
            return;
        }
        if (this->q.size() >= this->max_depth)
        {
            /* At capacity. Make room by evicting the oldest loss-tolerant
             * telemetry task first — a command dispatch/ack carries safety/audit
             * state and must never be discarded under telemetry pressure
             * (todo/20). */
            if (this->evict_oldest_telemetry())
            {
                dropped_telemetry = true;
                total_telemetry_dropped = this->dropped.fetch_add(1) + 1;
            }
            else if (is_command_task(task))
            {
                /* The whole queue is command writes and another command has
                 * arrived: genuine command overload, not telemetry pressure.
                 * Drop the oldest command to stay bounded, but on a distinct,
                 * observable path rather than the generic telemetry drop. */
                this->q.pop_front();
                dropped_command = true;
                total_command_dropped = this->command_dropped.fetch_add(1) + 1;
            }
            else
            {
                /* Incoming telemetry with no telemetry to evict (queue full of
                 * protected command writes): reject the new telemetry rather
                 * than evicting a command. Use the fetch_add result for the
                 * logged count, matching the eviction branch. */
                dropped_telemetry = true;
                total_telemetry_dropped = this->dropped.fetch_add(1) + 1;
                enqueued = false;
            }
        }
        if (enqueued)
        {
            this->q.push_back(task);
        }
    }
    if (enqueued)
    {
        this->cv.notify_one();
    }
    if (dropped_command)
    {
        /* Rate-limited error (first, then every 100th): a dropped command write
         * loses the link between a command and its ack, so it is far more
         * serious than a telemetry drop and must be loud. */
        constexpr uint64_t log_every = 100;
        if (total_command_dropped == 1 || (total_command_dropped % log_every) == 0)
        {
            FSS_LOG_ERROR("db-writer", "dropped a command DB write under command overload (total command dropped="
                                           << total_command_dropped << ")");
        }
    }
    else if (dropped_telemetry)
    {
        /* Rate-limited: first drop and every 1000th after that. */
        constexpr uint64_t log_every = 1000;
        if (total_telemetry_dropped == 1 || (total_telemetry_dropped % log_every) == 0)
        {
            FSS_LOG_WARN("db-writer",
                         "dropped oldest telemetry DB write (total dropped=" << total_telemetry_dropped << ")");
        }
    }
}

void db_write_queue::requestProbe()
{
    {
        std::scoped_lock guard(this->mtx);
        if (this->stopping)
        {
            return;
        }
        /* Stored under mtx so the flag cannot be set between the worker
         * evaluating its wait predicate and blocking on the cv. */
        this->probe_requested.store(true);
    }
    this->cv.notify_one();
}

void db_write_queue::stop()
{
    {
        std::scoped_lock guard(this->mtx);
        if (this->stopping)
        {
            return;
        }
        this->stopping = true;
    }
    this->cv.notify_all();
    if (this->worker.joinable())
    {
        try
        {
            this->worker.join();
        }
        catch (const std::system_error &e)
        {
            /* join() failed, so the thread is still joinable; leaving it would
             * make ~std::thread call std::terminate. Detach to avoid that
             * (leaking the thread) — we cannot recover the worker here. */
            FSS_LOG_ERROR("db-writer", "worker.join() failed, detaching: " << e.what());
            this->worker.detach();
            this->worker = std::thread();
        }
    }
}

auto db_write_queue::dropped_count() const -> uint64_t
{
    return this->dropped.load();
}

auto db_write_queue::command_dropped_count() const -> uint64_t
{
    return this->command_dropped.load();
}

auto db_write_queue::write_failure_count() const -> uint64_t
{
    return this->write_failures.load();
}

auto db_write_queue::probe_success_count() const -> uint64_t
{
    return this->probe_successes.load();
}

auto db_write_queue::probe_failure_count() const -> uint64_t
{
    return this->probe_failures.load();
}

auto db_write_queue::probe_inconclusive_count() const -> uint64_t
{
    return this->probe_inconclusive.load();
}

auto db_write_queue::last_probe_error() const -> std::string
{
    std::scoped_lock guard(this->probe_error_mtx);
    return this->probe_error;
}

/* pending_count() deliberately knows nothing about an outstanding probe: it is
 * the fail-safe's drain check, and a probe must never read as a backlog. */
auto db_write_queue::pending_count() const -> std::size_t
{
    std::scoped_lock guard(this->mtx);
    return this->q.size();
}

void db_write_queue::run_probe()
{
    try
    {
        if (this->probe())
        {
            ++this->probe_successes;
            return;
        }
        uint64_t inconclusive = ++this->probe_inconclusive;
        constexpr uint64_t log_every = 60;
        if (inconclusive == 1 || (inconclusive % log_every) == 0)
        {
            /* Not an error and not evidence: the fail-safe stays degraded on
             * it, so say why rather than leaving the operator with a silent
             * server that never recovers. */
            FSS_LOG_WARN("db-writer", "fail-safe probe write was inconclusive -- no assets registered, so the probe "
                                      "statement matched no row and proved nothing (total="
                                          << inconclusive << ")");
        }
    }
    catch (const std::exception &e)
    {
        uint64_t failures = ++this->probe_failures;
        {
            std::scoped_lock guard(this->probe_error_mtx);
            this->probe_error = e.what();
        }
        constexpr uint64_t log_every = 60;
        if (failures == 1 || (failures % log_every) == 0)
        {
            /* Rate-limited here and reported again, with elapsed time, by the
             * caller's degraded-state line; the probe runs about once a second
             * while degraded and could otherwise flood the log forever. */
            FSS_LOG_WARN("db-writer", "fail-safe probe write failed (total=" << failures << "): " << e.what());
        }
    }
    catch (...)
    {
        uint64_t failures = ++this->probe_failures;
        {
            std::scoped_lock guard(this->probe_error_mtx);
            this->probe_error = "unknown exception";
        }
        constexpr uint64_t log_every = 60;
        if (failures == 1 || (failures % log_every) == 0)
        {
            FSS_LOG_WARN("db-writer", "fail-safe probe write failed (total=" << failures << "): unknown exception");
        }
    }
}

void db_write_queue::run()
{
    for (;;)
    {
        db_write_task task;
        bool do_probe = false;
        {
            std::unique_lock<std::mutex> lock(this->mtx);
            this->cv.wait(
                lock, [this]() -> bool { return this->stopping || !this->q.empty() || this->probe_requested.load(); });
            if (this->stopping && this->q.empty())
            {
                /* An outstanding probe is abandoned rather than run: shutdown
                 * must not wait on the database, and nothing is left to
                 * recover for. */
                return;
            }
            if (this->q.empty())
            {
                /* Real writes take priority, and the probe only runs once the
                 * deque is empty, so its result describes the state *after*
                 * the backlog drained -- which is the state the fail-safe's
                 * recovery rule asks about. */
                do_probe = this->probe_requested.exchange(false);
                if (!do_probe)
                {
                    continue;
                }
            }
            else
            {
                task = this->q.front();
                this->q.pop_front();
            }
        }
        if (do_probe)
        {
            this->run_probe();
            continue;
        }
        try
        {
            this->sink(task);
        }
        catch (const std::exception &e)
        {
            uint64_t failures = ++this->write_failures;
            constexpr uint64_t log_every = 100;
            if (failures == 1 || (failures % log_every) == 0)
            {
                FSS_LOG_ERROR("db-writer", "sink failed (total=" << failures << "): " << e.what());
            }
        }
        catch (...)
        {
            uint64_t failures = ++this->write_failures;
            constexpr uint64_t log_every = 100;
            if (failures == 1 || (failures % log_every) == 0)
            {
                FSS_LOG_ERROR("db-writer", "sink failed (total=" << failures << "): unknown exception");
            }
        }
    }
}

} // namespace flight_safety_system::server
