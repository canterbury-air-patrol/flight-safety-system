#pragma once

/* Internal helper, not installed (like transport.hpp): a jsoncpp dependency in
 * an installed header would leak into the public API surface, and the .pc
 * files deliberately don't require jsoncpp for transport consumers. */

#include <cstdint>
#include <string>

#include "fss-log.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
#include <json/json.h>
#pragma GCC diagnostic pop

/* Shared TCP-port validator for a jsoncpp config node (todo/58): server.cpp's
 * `port`/`postgres.port` and client-ssl.cpp's per-server `servers[].port` used
 * to each carry their own copy of this isInt()/range check. `what` names the
 * field for the log message (e.g. "config field port" or "server config at
 * index 3"), so each call site keeps its own informative, distinct wording;
 * `component` is the FSS_LOG tag ("server"/"client") so log lines still read
 * as coming from the caller's subsystem. A missing member reaches here as
 * jsoncpp's null Json::Value (operator[] on a non-existent key), whose
 * isInt() is false, so the caller's missing-field path (log + skip/fail) is
 * unaffected without needing its own isMember() check. */
inline auto read_json_tcp_port(const Json::Value &node, const char *component, const std::string &what, uint16_t &out)
    -> bool
{
    constexpr int min_tcp_port = 1;
    constexpr int max_tcp_port = 65535;
    if (!node.isInt())
    {
        FSS_LOG_ERROR(component, "Invalid " << what << ": must be an integer TCP port");
        return false;
    }
    int port = node.asInt();
    if (port < min_tcp_port || port > max_tcp_port)
    {
        FSS_LOG_ERROR(component, "Invalid " << what << ": " << port << " is outside 1-65535");
        return false;
    }
    out = static_cast<uint16_t>(port);
    return true;
}
