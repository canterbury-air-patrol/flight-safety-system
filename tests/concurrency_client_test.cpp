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
#include <chrono>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "fss-transport.hpp"
#include "fss-server.hpp"
#include "db-write-queue.hpp"

using flight_safety_system::transport::fss_connection;
using flight_safety_system::transport::fss_message;
using flight_safety_system::transport::fss_message_identity;
using flight_safety_system::transport::fss_message_position_report;

namespace {

/* A connection whose recv thread reads injected bytes instead of a socket,
 * and which reports a fixed certificate CN so an fss_client can complete its
 * identify handshake. Deliberately self-contained so this TSan-only test does
 * not depend on the harness in concurrency_connection_test.cpp. */
class injecting_connection : public fss_connection {
    std::mutex bytes_lock{};
    std::queue<std::vector<uint8_t>> chunks{};
    std::atomic<bool> eof{false};
    std::list<std::string> names_;
public:
    explicit injecting_connection(std::list<std::string> names) : names_(std::move(names)) {}
    ~injecting_connection() override = default;
    injecting_connection(const injecting_connection &) = delete;
    injecting_connection(injecting_connection &&) = delete;
    auto operator=(const injecting_connection &) -> injecting_connection & = delete;
    auto operator=(injecting_connection &&) -> injecting_connection & = delete;

    auto getClientNames() -> std::list<std::string> override { return names_; }
    void push_bytes(const std::vector<uint8_t> &data)
    {
        std::scoped_lock lock(this->bytes_lock);
        this->chunks.push(data);
    }
    void close_input() { this->eof.store(true); }
    /* Start the recv thread the same way fss_connection::create() does. */
    void start()
    {
        this->startRecvThread(std::thread([this]() -> void { this->processMessages(); }));
    }
protected:
    auto recvBytes(void *t_bytes, size_t t_max_bytes) -> ssize_t override
    {
        for (;;)
        {
            {
                std::scoped_lock lock(this->bytes_lock);
                if (!this->chunks.empty())
                {
                    auto &chunk = this->chunks.front();
                    size_t n = std::min(t_max_bytes, chunk.size());
                    memcpy(t_bytes, chunk.data(), n);
                    if (n == chunk.size())
                    {
                        this->chunks.pop();
                    }
                    else
                    {
                        chunk.erase(chunk.begin(), chunk.begin() + static_cast<long>(n));
                    }
                    return static_cast<ssize_t>(n);
                }
                if (this->eof.load())
                {
                    return 0;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

/* Minimal IDatabase: knows one aircraft ("craft" -> id 1) and does nothing
 * else. No real database is involved. */
class stub_database : public flight_safety_system::server::IDatabase {
public:
    auto getAssetId(const std::string &name) -> std::optional<uint64_t> override
    {
        return uint64_t{name == "craft" ? 1U : 0U};
    }
    void recordPosition(uint64_t, double, double, uint32_t) override {}
    void recordRtt(uint64_t, uint64_t) override {}
    void recordStatus(uint64_t, uint8_t, uint32_t, double) override {}
    void recordSearchStatus(uint64_t, uint64_t, uint64_t, uint64_t) override {}
    void recordCommandDispatch(uint64_t, uint64_t) override {}
    void recordCommandAck(uint64_t, uint64_t, uint8_t, uint64_t, uint8_t) override {}
    auto getCommand(uint64_t) -> std::optional<std::shared_ptr<flight_safety_system::server::asset_command>> override
    {
        /* An engaged null pointer: no pending command, not a failed read. */
        return nullptr;
    }
    auto getCommands(const std::vector<uint64_t> &) -> std::optional<
        std::unordered_map<uint64_t, std::shared_ptr<flight_safety_system::server::asset_command>>> override
    {
        /* An engaged empty map: nobody has a pending command, not a failed read. */
        return std::unordered_map<uint64_t, std::shared_ptr<flight_safety_system::server::asset_command>>{};
    }
    auto getActiveServers() -> std::optional<std::vector<flight_safety_system::server::fss_server_details>> override
    {
        /* An engaged empty vector: no servers configured, not a failed read. */
        return std::vector<flight_safety_system::server::fss_server_details>{};
    }
    auto getSmmSettings(uint64_t) -> std::optional<std::shared_ptr<flight_safety_system::server::smm_settings>> override
    {
        /* An engaged null pointer: no settings configured, not a failed read. */
        return nullptr;
    }
    auto isConnected() const -> bool override { return true; }
    void tryReconnectIfNeeded() override {}
};

class null_handler : public flight_safety_system::server::fss_client_handler {
public:
    void clientDisconnected(flight_safety_system::server::fss_client *) override {}
    void broadcastMsg(const std::shared_ptr<fss_message> &, flight_safety_system::server::fss_client *) override {}
};

auto framed(const std::shared_ptr<fss_message> &msg) -> std::vector<uint8_t>
{
    auto bl = msg->getPacked();
    const char *data = bl->getData();
    return {data, data + bl->getLength()};
}

auto make_position() -> std::shared_ptr<fss_message>
{
    return std::make_shared<fss_message_position_report>(0.0, 0.0, 0U, 0U, 0U, int16_t{0}, 0U, std::string{}, 0U,
                                                         uint8_t{0}, 0U, uint8_t{0}, uint8_t{0}, uint64_t{0});
}

} // namespace

/* Regression guard for the activate()-ordering fix: per-client config must be
 * applied before the connection's message handler is wired, otherwise the
 * already-running recv thread can call processMessage (touching the
 * non-atomic msg_rate limiter) concurrently with setRateLimits(). If
 * activation were moved back into the constructor, the recv thread below —
 * primed with an identity message and a burst of telemetry — would deliver
 * into processMessage and race the setters, which ThreadSanitizer would flag. */
TEST_CASE("tsan: fss_client config precedes handler activation")
{
    namespace srv = flight_safety_system::server;

    auto conn = std::make_shared<injecting_connection>(std::list<std::string>{"craft"});
    /* Identity first so the client reaches the identified state where
     * msg_rate.consume() is exercised by subsequent telemetry. */
    conn->push_bytes(framed(std::make_shared<fss_message_identity>("craft")));
    conn->start();

    /* A background feeder streams telemetry continuously so the recv thread is
     * actively delivering into processMessage *while* the main thread applies
     * config below. With the activate() fix the handler is wired only after
     * config, so these never overlap; if activation were moved back into the
     * constructor, the recv thread would call msg_rate.consume() concurrently
     * with setRateLimits() and ThreadSanitizer would report the race. The
     * overlap (not a fixed pre-queued burst) is what makes this a real guard. */
    std::atomic<bool> feeding{true};
    std::thread feeder([&conn, &feeding]() -> void {
        while (feeding.load())
        {
            conn->push_bytes(framed(make_position()));
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    stub_database db;
    srv::db_write_sink sink = [](const srv::db_write_task &) -> void {};
    auto writer = std::make_shared<srv::db_write_queue>(std::size_t{1024}, sink);
    null_handler handler;

    auto client = std::make_shared<srv::fss_client>(conn, &db, writer, &handler);
    /* Production ordering (mirrors server_clients::clientConnected): config
     * first, then activate. We hammer setRateLimits in a tight window to widen
     * the race detection — under the fix the handler is not yet wired so the
     * streaming recv thread buffers (never touching msg_rate), making this
     * loop single-threaded and clean; with activation in the constructor the
     * recv thread would be calling msg_rate.consume() throughout this loop. */
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < deadline)
    {
        client->setRateLimits(50, 10);
        client->setTimeoutMs(5000);
    }
    client->activate();

    /* Let the recv thread churn through telemetry under the wired handler. */
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    feeding.store(false);
    feeder.join();
    conn->close_input();
    client->disconnect();
    writer->stop();

    SUCCEED("no data race detected by ThreadSanitizer");
}
