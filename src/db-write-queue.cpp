#include "db-write-queue.hpp"
#include "fss-log.hpp"

#include <algorithm>
#include <exception>
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

void db_write_queue::enqueue(db_write_task task)
{
    bool did_drop = false;
    uint64_t total_dropped = 0;
    {
        std::scoped_lock guard(this->mtx);
        if (this->stopping)
        {
            return;
        }
        if (this->q.size() >= this->max_depth)
        {
            this->q.pop_front();
            total_dropped = this->dropped.fetch_add(1) + 1;
            did_drop = true;
        }
        this->q.push_back(task);
    }
    this->cv.notify_one();
    if (did_drop)
    {
        /* Rate-limited: first drop and every 1000th after that. All current
         * tasks are telemetry; if command writes are added later they should
         * bypass this policy. */
        constexpr uint64_t log_every = 1000;
        if (total_dropped == 1 || (total_dropped % log_every) == 0)
        {
            FSS_LOG_WARN("db-writer", "dropped oldest DB write (total dropped=" << total_dropped << ")");
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
        catch (const std::exception &e)
        {
            FSS_LOG_ERROR("db-writer", "worker.join() failed: " << e.what());
        }
    }
}

auto db_write_queue::dropped_count() const -> uint64_t
{
    return this->dropped.load();
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
