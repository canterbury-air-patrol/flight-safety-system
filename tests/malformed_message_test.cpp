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

#include <memory>
#include <string>

#include <sys/socket.h>
#include <unistd.h>

#include "fss-transport.hpp"
#include "test_helpers.hpp"

using flight_safety_system::transport::buf_len;
using flight_safety_system::transport::fss_message;
using flight_safety_system::transport::fss_message_identity;
using flight_safety_system::transport::fss_message_identity_non_aircraft;
using flight_safety_system::transport::fss_message_identity_required;
using flight_safety_system::transport::fss_message_position_report;
using flight_safety_system::transport::fss_message_rtt_request;
using flight_safety_system::transport::fss_message_rtt_response;
using flight_safety_system::transport::fss_message_search_status;
using flight_safety_system::transport::fss_message_asset_command;
using flight_safety_system::transport::fss_message_server_list;
using flight_safety_system::transport::fss_message_smm_settings;
using flight_safety_system::transport::fss_message_system_status;
using flight_safety_system::transport::asset_command_rtl;
using flight_safety_system::transport::asset_command_unknown;
using flight_safety_system::transport::message_type_closed;
using flight_safety_system::transport::message_type_command;
using flight_safety_system::transport::message_type_identity;
using flight_safety_system::transport::message_type_identity_non_aircraft;
using flight_safety_system::transport::message_type_identity_required;
using flight_safety_system::transport::message_type_position_report;
using flight_safety_system::transport::message_type_rtt_request;
using flight_safety_system::transport::message_type_rtt_response;
using flight_safety_system::transport::message_type_search_status;
using flight_safety_system::transport::message_type_server_list;
using flight_safety_system::transport::message_type_smm_settings;
using flight_safety_system::transport::message_type_system_status;
using flight_safety_system::transport::message_type_unknown;

TEST_CASE("malformed: undecodable frame stream is log-throttled and survivable")
{
    /* A connected peer streaming garbage frames used to emit one WARN per
     * frame — unbounded log growth at line rate.  The log is now throttled
     * to the first frame and every 100th after, the connection stays up,
     * and a subsequent valid message resets the counter and is delivered. */
    fss_test::capture_cerr capture; /* installed before the recv thread starts */
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    fss_test::scoped_fd peer(fds[1]);
    auto conn = flight_safety_system::transport::fss_connection::create(fds[0]);

    /* Unknown-type frame, declared length 16 (8-byte aligned so recvMsg
     * consumes exactly what is written): header (12) + 4 padding bytes. */
    constexpr uint64_t frames = 250;
    auto garbage = fss_test::make_framed_buffer(0x00FFU, 1, std::string(4, '\0'), 16);
    for (uint64_t i = 0; i < frames; i++)
    {
        REQUIRE(::write(peer.get(), garbage->getData(), garbage->getLength()) ==
                static_cast<ssize_t>(garbage->getLength()));
    }
    REQUIRE(fss_test::wait_for([&]() { return conn->getNullMsgCount() >= frames; }));
    REQUIRE(conn->getNullMsgCount() == frames);

    /* A valid message is still delivered and resets the counter. */
    auto ident = std::make_shared<fss_message_identity>("recovers");
    ident->setId(2);
    auto good = ident->getPacked();
    REQUIRE(::write(peer.get(), good->getData(), good->getLength()) == static_cast<ssize_t>(good->getLength()));
    std::shared_ptr<fss_message> msg;
    REQUIRE(fss_test::wait_for([&]() {
        msg = conn->getMsg();
        return msg != nullptr;
    }));
    REQUIRE(msg->getType() == message_type_identity);
    REQUIRE(conn->getNullMsgCount() == 0);

    conn->disconnect();

    /* 250 garbage frames log at 1, 100, and 200 — three lines, not 250. */
    const std::string logged = capture.str();
    std::size_t occurrences = 0;
    for (std::size_t pos = logged.find("Got a null msg"); pos != std::string::npos;
         pos = logged.find("Got a null msg", pos + 1))
    {
        occurrences++;
    }
    REQUIRE(occurrences == 3);
}

TEST_CASE("malformed: valid message mid-stream re-arms the log throttle")
{
    /* The throttle counter resets on every successfully decoded message, so
     * a second garbage burst must log from its own frame 1 again - proving
     * an honest peer interleaving valid traffic is never starved of the
     * warning, and that the counter genuinely resets. */
    fss_test::capture_cerr capture;
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    fss_test::scoped_fd peer(fds[1]);
    auto conn = flight_safety_system::transport::fss_connection::create(fds[0]);

    constexpr uint64_t burst = 150; /* logs at 1 and 100 */
    auto garbage = fss_test::make_framed_buffer(0x00FFU, 1, std::string(4, '\0'), 16);
    auto send_burst = [&]() -> void {
        for (uint64_t i = 0; i < burst; i++)
        {
            REQUIRE(::write(peer.get(), garbage->getData(), garbage->getLength()) ==
                    static_cast<ssize_t>(garbage->getLength()));
        }
        REQUIRE(fss_test::wait_for([&]() { return conn->getNullMsgCount() >= burst; }));
    };

    send_burst();

    /* Valid message: delivered, resets the counter. */
    auto ident = std::make_shared<fss_message_identity>("midstream");
    ident->setId(2);
    auto good = ident->getPacked();
    REQUIRE(::write(peer.get(), good->getData(), good->getLength()) == static_cast<ssize_t>(good->getLength()));
    std::shared_ptr<fss_message> msg;
    REQUIRE(fss_test::wait_for([&]() {
        msg = conn->getMsg();
        return msg != nullptr;
    }));
    REQUIRE(msg->getType() == message_type_identity);
    REQUIRE(conn->getNullMsgCount() == 0);

    send_burst();
    conn->disconnect();

    /* Two bursts of 150: throttle fires at 1 and 100 in each = 4 lines. */
    const std::string logged = capture.str();
    std::size_t occurrences = 0;
    for (std::size_t pos = logged.find("Got a null msg"); pos != std::string::npos;
         pos = logged.find("Got a null msg", pos + 1))
    {
        occurrences++;
    }
    REQUIRE(occurrences == 4);
}

/* The disconnect threshold lives in src/fss-transport.hpp as a private
 * static constexpr (null_msg_disconnect_threshold). It is not part of the
 * public surface, so mirror its value here; if it changes, these tests must
 * be updated alongside it. */
static constexpr uint64_t kNullMsgDisconnectThreshold = 1000;

TEST_CASE("malformed: a garbage-only stream is disconnected after the threshold")
{
    /* A peer streaming nothing but undecodable frames must be closed once it
     * crosses null_msg_disconnect_threshold consecutive nulls, rather than
     * being logged at line rate forever. With no handler installed the
     * synthesised close is delivered via the message queue. */
    fss_test::capture_cerr capture;
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    fss_test::scoped_fd peer(fds[1]);
    auto conn = flight_safety_system::transport::fss_connection::create(fds[0]);

    /* Write the whole burst up front (threshold worth of 16-byte frames is a
     * few KiB, well within the socketpair buffer) so the recv thread can drain
     * it without the writer blocking. The recv thread closes our fd at the
     * threshold, so later writes may fail with EPIPE - SIGPIPE is ignored in
     * tests/main.cpp, so tolerate that rather than REQUIRE-ing each write. */
    auto garbage = fss_test::make_framed_buffer(0x00FFU, 1, std::string(4, '\0'), 16);
    for (uint64_t i = 0; i < kNullMsgDisconnectThreshold + 16; i++)
    {
        if (::write(peer.get(), garbage->getData(), garbage->getLength()) != static_cast<ssize_t>(garbage->getLength()))
        {
            break; /* recv side closed the connection - threshold reached */
        }
    }

    std::shared_ptr<fss_message> msg;
    REQUIRE(fss_test::wait_for([&]() {
        msg = conn->getMsg();
        return msg != nullptr;
    }));
    REQUIRE(msg->getType() == message_type_closed);

    /* One ERROR line announces the close; the WARN throttle keeps the rest
     * bounded (it does not grow with the number of frames). */
    const std::string logged = capture.str();
    REQUIRE(logged.find("consecutive undecodable frames, closing") != std::string::npos);
}

TEST_CASE("malformed: a peer just below the threshold is not disconnected")
{
    /* Boundary: threshold - 1 consecutive nulls must leave the session up,
     * and a following valid message resets the counter and is delivered. */
    fss_test::capture_cerr capture;
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    fss_test::scoped_fd peer(fds[1]);
    auto conn = flight_safety_system::transport::fss_connection::create(fds[0]);

    constexpr uint64_t nulls = kNullMsgDisconnectThreshold - 1;
    auto garbage = fss_test::make_framed_buffer(0x00FFU, 1, std::string(4, '\0'), 16);
    for (uint64_t i = 0; i < nulls; i++)
    {
        REQUIRE(::write(peer.get(), garbage->getData(), garbage->getLength()) ==
                static_cast<ssize_t>(garbage->getLength()));
    }
    REQUIRE(fss_test::wait_for([&]() { return conn->getNullMsgCount() >= nulls; }));
    REQUIRE(conn->getNullMsgCount() == nulls);

    auto ident = std::make_shared<fss_message_identity>("survivor");
    ident->setId(2);
    auto good = ident->getPacked();
    REQUIRE(::write(peer.get(), good->getData(), good->getLength()) == static_cast<ssize_t>(good->getLength()));
    std::shared_ptr<fss_message> msg;
    REQUIRE(fss_test::wait_for([&]() {
        msg = conn->getMsg();
        return msg != nullptr;
    }));
    REQUIRE(msg->getType() == message_type_identity);
    REQUIRE(conn->getNullMsgCount() == 0);

    conn->disconnect();
    REQUIRE(capture.str().find("consecutive undecodable frames, closing") == std::string::npos);
}

TEST_CASE("malformed: valid traffic interleaved with garbage keeps the session alive")
{
    /* Forward-compat: a version-skewed peer sends the odd unknown message type
     * interleaved with valid traffic. Each valid message resets the
     * consecutive-null counter, so two near-threshold bursts separated by a
     * valid message never trip the disconnect even though their sum far
     * exceeds the threshold. */
    fss_test::capture_cerr capture;
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    fss_test::scoped_fd peer(fds[1]);
    auto conn = flight_safety_system::transport::fss_connection::create(fds[0]);

    constexpr uint64_t burst = kNullMsgDisconnectThreshold - 1;
    auto garbage = fss_test::make_framed_buffer(0x00FFU, 1, std::string(4, '\0'), 16);
    auto send_garbage_then_valid = [&](const std::string &name, uint64_t id) -> void {
        for (uint64_t i = 0; i < burst; i++)
        {
            REQUIRE(::write(peer.get(), garbage->getData(), garbage->getLength()) ==
                    static_cast<ssize_t>(garbage->getLength()));
        }
        auto ident = std::make_shared<fss_message_identity>(name);
        ident->setId(id);
        auto good = ident->getPacked();
        REQUIRE(::write(peer.get(), good->getData(), good->getLength()) == static_cast<ssize_t>(good->getLength()));
        std::shared_ptr<fss_message> msg;
        REQUIRE(fss_test::wait_for([&]() {
            msg = conn->getMsg();
            return msg != nullptr;
        }));
        REQUIRE(msg->getType() == message_type_identity);
    };

    send_garbage_then_valid("first", 2);
    send_garbage_then_valid("second", 3);

    conn->disconnect();
    /* 2 * (threshold - 1) nulls total, but never threshold consecutive. */
    REQUIRE(capture.str().find("consecutive undecodable frames, closing") == std::string::npos);
}

TEST_CASE("malformed: decode returns nullptr for buffer shorter than header")
{
    /* Header is 2 + 2 + 8 = 12 bytes. Anything shorter must not crash. */
    auto bl = std::make_shared<buf_len>("\x00", 1);
    REQUIRE(fss_message::decode(bl) == nullptr);

    std::string eleven(11, '\0');
    auto bl2 = std::make_shared<buf_len>(eleven.data(), static_cast<uint16_t>(eleven.size()));
    REQUIRE(fss_message::decode(bl2) == nullptr);
}

TEST_CASE("malformed: decode returns nullptr for empty buffer")
{
    auto bl = std::make_shared<buf_len>();
    REQUIRE(fss_message::decode(bl) == nullptr);
}

TEST_CASE("malformed: decode returns nullptr for unknown message type")
{
    /* 0x00FF is an unused enum value; decode must not crash. */
    auto bl = fss_test::make_framed_buffer(0x00FFU, 42, "", 12);
    REQUIRE(fss_message::decode(bl) == nullptr);
}

TEST_CASE("malformed: decode validates the wire type before casting (todo/40)")
{
    /* fss_message_type is an unscoped enum with no fixed underlying type, so
     * its representable range is only the smallest bit-field that covers its
     * enumerators (0..15 today, for the 15 values 0..14) — casting an
     * arbitrary untrusted uint16_t straight into it is UB per [dcl.enum]/8
     * for anything past that range (e.g. 5000). decode_message_type() must
     * map every value that isn't a defined enumerator to message_type_unknown
     * (undecodable) rather than ever performing that cast — checked here one
     * past the last enumerator (in-range but still not a defined value), a
     * mid-range unused byte, and the full 16-bit maximum (genuinely UB to
     * cast). */
    REQUIRE(fss_message::decode(fss_test::make_framed_buffer(15U, 1, "", 12)) == nullptr);
    REQUIRE(fss_message::decode(fss_test::make_framed_buffer(255U, 2, "", 12)) == nullptr);
    REQUIRE(fss_message::decode(fss_test::make_framed_buffer(0xFFFFU, 3, "", 12)) == nullptr);
}

TEST_CASE("malformed: decode returns nullptr for message_type_unknown")
{
    auto bl = fss_test::make_framed_buffer(static_cast<uint16_t>(message_type_unknown), 1, "", 12);
    REQUIRE(fss_message::decode(bl) == nullptr);
}

TEST_CASE("malformed: wrong-type dynamic_pointer_cast returns nullptr, no UB")
{
    /* Regression for todo/13-dynamic-cast-null-checks.md:
     * a message decoded as identity must not be cast-convertible to an
     * unrelated subclass; the cast should produce nullptr and the caller
     * must observe it without crashing. */
    auto original = std::make_shared<fss_message_identity>("name");
    original->setId(7);
    auto bl = original->getPacked();
    auto decoded = fss_message::decode(bl);
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_identity);

    auto wrong_cast = std::dynamic_pointer_cast<fss_message_position_report>(decoded);
    REQUIRE(wrong_cast == nullptr);
}

TEST_CASE("malformed: zero-length string field decodes to empty string")
{
    auto original = std::make_shared<fss_message_identity>("");
    original->setId(1);
    auto bl = original->getPacked();
    auto decoded = std::make_shared<fss_message_identity>(1, bl);
    REQUIRE(decoded->getName().empty());
}

/* Regression for todo/13: decode() must return a message whose getType()
 * matches the type field in the header.  These round-trip each concrete
 * message class through getPacked() → decode() and verify the invariant. */
TEST_CASE("decode type consistency: identity")
{
    auto orig = std::make_shared<fss_message_identity>("asset");
    orig->setId(1);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_identity);
    REQUIRE(std::dynamic_pointer_cast<fss_message_identity>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: rtt_request")
{
    auto orig = std::make_shared<fss_message_rtt_request>();
    orig->setId(2);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_rtt_request);
    REQUIRE(std::dynamic_pointer_cast<fss_message_rtt_request>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: rtt_response")
{
    auto orig = std::make_shared<fss_message_rtt_response>(99ULL);
    orig->setId(3);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_rtt_response);
    REQUIRE(std::dynamic_pointer_cast<fss_message_rtt_response>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: system_status")
{
    auto orig = std::make_shared<fss_message_system_status>(80, 1200, 12.4);
    orig->setId(4);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_system_status);
    REQUIRE(std::dynamic_pointer_cast<fss_message_system_status>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: search_status")
{
    auto orig = std::make_shared<fss_message_search_status>(1ULL, 50ULL, 100ULL);
    orig->setId(5);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_search_status);
    REQUIRE(std::dynamic_pointer_cast<fss_message_search_status>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: server_list")
{
    auto orig = std::make_shared<fss_message_server_list>();
    orig->setId(6);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_server_list);
    REQUIRE(std::dynamic_pointer_cast<fss_message_server_list>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: identity_non_aircraft")
{
    auto orig = std::make_shared<fss_message_identity_non_aircraft>();
    orig->setId(7);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_identity_non_aircraft);
    REQUIRE(std::dynamic_pointer_cast<fss_message_identity_non_aircraft>(decoded) != nullptr);
}

TEST_CASE("malformed: string alignment overshoot does not read past the buffer")
{
    /* Regression for a heap over-read: unpackString rounds the offset up to
     * an 8-byte boundary, which can push it past the declared buffer length;
     * BufferReader::ensureAvailable then subtracted (length - offset) as
     * size_t and wrapped, admitting an out-of-bounds read on the following
     * field. Craft a server_list whose single address ends so the alignment
     * overshoots, then the loop attempts to read the next port (the
     * underflow site). Pre-fix this aborts under ASan; post-fix the loop
     * ends cleanly with exactly the one decoded server.
     *
     * Layout after the 12-byte header: port(2) + len(2) + 6 addr bytes = 22
     * total. offset reaches 22, aligns up to 24 (> 22). */
    std::string payload;
    const uint16_t port_n = htons(1);
    const uint16_t len_n = htons(6);
    payload.append(reinterpret_cast<const char *>(&port_n), sizeof(port_n));
    payload.append(reinterpret_cast<const char *>(&len_n), sizeof(len_n));
    payload.append("abcdef", 6);
    REQUIRE(payload.size() == 10);

    auto framed = fss_test::make_framed_buffer(static_cast<uint16_t>(message_type_server_list), 1, payload,
                                               static_cast<uint16_t>(12 + payload.size()));
    /* Re-wrap at EXACT size, as recvMsg does: make_framed_buffer grows its
     * string via append() and leaves spare capacity that would absorb the
     * over-read, whereas the real receive path builds the buffer from the
     * wire at precisely its length. */
    auto bl = std::make_shared<buf_len>(framed->getData(), static_cast<uint16_t>(framed->getLength()));
    auto decoded = fss_message::decode(bl);
    REQUIRE(decoded != nullptr);
    auto sl = std::dynamic_pointer_cast<fss_message_server_list>(decoded);
    REQUIRE(sl != nullptr);
    auto servers = sl->getServers();
    REQUIRE(servers.size() == 1);
    REQUIRE(servers[0].first == "abcdef");
    REQUIRE(servers[0].second == 1);
}

TEST_CASE("decode type consistency: position_report")
{
    auto orig =
        std::make_shared<fss_message_position_report>(-33.8688, 151.2093, 100, 0, 0, 0, 0, "TEST", 0, 0, 0, 0, 0, 0ULL);
    orig->setId(11);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_position_report);
    REQUIRE(std::dynamic_pointer_cast<fss_message_position_report>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: command")
{
    auto orig = std::make_shared<fss_message_asset_command>(asset_command_rtl, 0ULL);
    orig->setId(9);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_command);
    REQUIRE(std::dynamic_pointer_cast<fss_message_asset_command>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: smm_settings")
{
    auto orig = std::make_shared<fss_message_smm_settings>(
        "http://example.com", flight_safety_system::secure_string(std::string_view("user")),
        flight_safety_system::secure_string(std::string_view("pass")));
    orig->setId(10);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_smm_settings);
    REQUIRE(std::dynamic_pointer_cast<fss_message_smm_settings>(decoded) != nullptr);
}

TEST_CASE("decode type consistency: identity_required")
{
    auto orig = std::make_shared<fss_message_identity_required>();
    orig->setId(8);
    auto decoded = fss_message::decode(orig->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getType() == message_type_identity_required);
    REQUIRE(std::dynamic_pointer_cast<fss_message_identity_required>(decoded) != nullptr);
}

/* Regression for UB in fss_message_asset_command::unpackData: casting an
 * out-of-range wire byte to fss_asset_command was undefined behaviour.
 * After the fix decode_asset_command() validates the byte first and maps
 * any unrecognised value to asset_command_unknown. */
TEST_CASE("malformed: out-of-range asset command byte decodes to asset_command_unknown")
{
    /* Build a valid asset_command message via getPacked(), then replace the
     * last byte (the command byte on the wire) with an out-of-range value.
     * Copy the buffer into a mutable std::string first so the modification
     * is well-defined (getData() returns const char *). */
    auto orig = std::make_shared<fss_message_asset_command>(asset_command_rtl, 0ULL);
    orig->setId(1);
    auto packed = orig->getPacked();

    std::string wire(packed->getData(), packed->getLength());
    /* The command byte follows the framing header and the fixed asset_command
     * payload (timestamp, lat, lng, altitude). Derive its offset from the wire
     * field widths via sizeof so this stays correct if a field width changes;
     * the buffer is padded to a multiple of 8 bytes, so the trailing padding
     * must not be confused with the command byte. */
    static constexpr size_t header_len = 12U;
    static constexpr size_t cmd_offset =
        header_len + sizeof(uint64_t) + sizeof(int32_t) + sizeof(int32_t) + sizeof(uint32_t);
    REQUIRE(wire.size() > cmd_offset);
    /* 200 is not a defined fss_asset_command enumerator. */
    wire[cmd_offset] = static_cast<char>(200);

    auto bl = std::make_shared<buf_len>(wire.data(), static_cast<uint16_t>(wire.size()));
    auto decoded = std::dynamic_pointer_cast<fss_message_asset_command>(fss_message::decode(bl));
    REQUIRE(decoded != nullptr);
    REQUIRE(decoded->getCommand() == asset_command_unknown);
}

/* Regression for todo/13: a cast to the wrong subclass must yield nullptr,
 * not a non-null pointer to a mismatched object — checked for every type. */
TEST_CASE("decode type consistency: wrong cast always returns nullptr")
{
    auto id_msg = std::make_shared<fss_message_identity>("x");
    id_msg->setId(1);
    auto decoded = fss_message::decode(id_msg->getPacked());
    REQUIRE(decoded != nullptr);
    REQUIRE(std::dynamic_pointer_cast<fss_message_rtt_response>(decoded) == nullptr);
    REQUIRE(std::dynamic_pointer_cast<fss_message_position_report>(decoded) == nullptr);
    REQUIRE(std::dynamic_pointer_cast<fss_message_system_status>(decoded) == nullptr);
    REQUIRE(std::dynamic_pointer_cast<fss_message_search_status>(decoded) == nullptr);
    REQUIRE(std::dynamic_pointer_cast<fss_message_server_list>(decoded) == nullptr);
}

/* Defensive: the length field in the header claims more bytes than the
 * buffer actually contains. decode() must not read past the buffer. */
TEST_CASE("malformed: oversized declared length does not read past buffer")
{
    /* Declare 0xFFFF in the length prefix but only provide a header + a tiny
     * payload. The identity decode path reads the declared length as the
     * string size and will over-read if the bug is not fixed. */
    std::string tiny_payload(4, 'A');
    auto bl = fss_test::make_framed_buffer(static_cast<uint16_t>(message_type_identity), 1, tiny_payload,
                                           /* declared_length */ 0xFFFFU);

    /* Correct behaviour: decode rejects the buffer (returns nullptr) or
     * returns a message whose name is bounded by the actual payload. */
    auto decoded = fss_message::decode(bl);
    if (decoded == nullptr)
    {
        SUCCEED("rejected oversized length");
    }
    else
    {
        auto ident = std::dynamic_pointer_cast<fss_message_identity>(decoded);
        REQUIRE(ident != nullptr);
        REQUIRE(ident->getName().size() <= tiny_payload.size());
    }
}
