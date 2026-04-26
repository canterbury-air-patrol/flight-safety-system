#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <variant>

namespace flight_safety_system {
namespace server {

struct rtt_write            { uint64_t asset_id; uint64_t rtt_ms; };
struct position_write       { uint64_t asset_id; double latitude; double longitude; uint32_t altitude; };
struct status_write         { uint64_t asset_id; uint8_t bat_percent; uint32_t bat_mah_used; double bat_voltage; };
struct search_status_write  { uint64_t asset_id; uint64_t search_id; uint64_t completed; uint64_t total; };

using db_write_task = std::variant<rtt_write, position_write, status_write, search_status_write>;
using db_write_sink = std::function<void(const db_write_task &)>;

/* C++17 overload helper for std::visit. */
template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

class db_write_queue {
private:
    std::size_t max_depth;
    db_write_sink sink;
    mutable std::mutex mtx{};
    std::condition_variable cv{};
    std::deque<db_write_task> q{};
    bool stopping{false};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> write_failures{0};
    std::thread worker{};

    void run();

public:
    db_write_queue(std::size_t t_max_depth, db_write_sink t_sink);
    ~db_write_queue();
    db_write_queue(const db_write_queue &) = delete;
    db_write_queue(db_write_queue &&) = delete;
    auto operator=(const db_write_queue &) -> db_write_queue & = delete;
    auto operator=(db_write_queue &&) -> db_write_queue & = delete;

    void enqueue(db_write_task task);
    void stop();
    auto dropped_count() const -> uint64_t;
    auto write_failure_count() const -> uint64_t;
    auto pending_count() const -> std::size_t;
};

} // namespace server
} // namespace flight_safety_system
