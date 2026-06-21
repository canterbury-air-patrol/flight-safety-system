#include "db-write-queue.hpp"
#include "fss-log.hpp"

#include <algorithm>
#include <exception>
#include <system_error>
#include <thread>
#include <utility>

namespace flight_safety_system::server {

db_write_queue::db_write_queue(std::size_t t_max_depth, db_write_sink t_sink)
    : max_depth(std::max<std::size_t>(1, t_max_depth)), sink(std::move(t_sink))
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

auto db_write_queue::pending_count() const -> std::size_t
{
    std::scoped_lock guard(this->mtx);
    return this->q.size();
}

void db_write_queue::run()
{
    for (;;)
    {
        db_write_task task;
        {
            std::unique_lock<std::mutex> lock(this->mtx);
            this->cv.wait(lock, [this]() -> bool { return this->stopping || !this->q.empty(); });
            if (this->stopping && this->q.empty())
            {
                return;
            }
            task = this->q.front();
            this->q.pop_front();
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
