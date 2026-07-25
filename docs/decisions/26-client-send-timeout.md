# 26 — Client send timeouts use `TCP_USER_TIMEOUT`, not `SO_SNDTIMEO`

**Decided** 2026-07-16. Shipped in 1.2.0. Config field `tcp_user_timeout_ms`
in the client JSON; default `default_tcp_user_timeout_ms` (30 s) in
`src/fss-transport.hpp`.

## Context

`sendMsg()` writes to a blocking socket. Against a half-dead peer — one whose
host has vanished, or whose receive window has been closed for good — that write
can block for as long as the kernel keeps retransmitting. A flight-safety client
needs a tighter, configurable bound than the server's 30 s liveness envelope.

## Decision

Bound it per connection with **`TCP_USER_TIMEOUT`**: how long transmitted data
may stay unacknowledged before the kernel errors the connection out.
`fss_server::reconnect_to()` reads the configured value at every (re)connect —
so it also covers servers learned from a server-list update — and threads it
through into `fss_connection::setTcpUserTimeoutMs()`, applied by both
`connectTo()` paths.

`set_tcp_keepalive(fd, ms)` deliberately has **no default argument**: every call
site states its own bound, and the server accept path passes
`default_tcp_user_timeout_ms` explicitly.

## Alternative deliberately not taken: `SO_SNDTIMEO`

Two independent reasons:

1. A timed-out `send()` can return after a **partial** write, leaving a
   half-frame on the stream. The connection is unrecoverable at that point —
   which is exactly what `TCP_USER_TIMEOUT` already delivers, without breaking
   the blocking-socket invariant both `sendMsg()` loops rely on.
2. On Linux `TCP_USER_TIMEOUT` also bounds the zero-window case (peer alive but
   not reading), so `SO_SNDTIMEO` adds no coverage it lacks.

## Where this is pinned

The 30 s default pin test also pins that a caller-supplied bound is applied
verbatim; plain-TCP and TLS `connectTo` each pin that the option lands on the
socket, via fd-exposing test subclasses; client JSON config parsing is covered
for the field present and absent.
