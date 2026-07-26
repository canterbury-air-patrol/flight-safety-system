# 66/67 — the client fans out through per-server workers, and prunes what it learns

**Decided** 2026-07-26. Config fields `learned_server_expiry_ms` and
`max_learned_servers` in the client JSON; defaults
`default_learned_server_expiry_ms` (60 s) and `default_max_learned_servers`
(16) in `src/fss-client-ssl.hpp`. Broke the ABI of `libfss-client-ssl`; all
four sonames bumped in lockstep (see `../release-checklist.md`).

## Context

The server was hardened over four work items — todo/21 (per-client outbound
worker threads), todo/35 (no blocking send under the client lock), todo/36
(broadcasts and position relay routed onto those workers, bounded drop-oldest
queues) and todo/55 (byte-clone broadcast) — around a single principle:

> a black-holed peer must never stall anything but itself.

`client_ssl::fss_client`, the aircraft side and the library cap-fmu builds on,
had the opposite structure. Every fan-out was a serial loop of blocking calls on
the caller's thread:

- `sendMsgAll()` called `sendMsg()` on each server in turn. A server that
  completes TCP and TLS and then stops reading — closed receive window, frozen
  host — blocks that send until `TCP_USER_TIMEOUT` errors the connection out
  (30 s by default, see decision 26). Every healthy server behind it in the list
  got nothing for that whole window.
- `attemptReconnect()` dialled each pending server in turn: a blocking
  `connect()` plus a TLS handshake (~7 s for a host that drops SYNs, up to 10 s
  for one that accepts TCP and then stalls the handshake), plus a
  `disconnect()` that joins the previous connection's recv thread. A pass cost
  the **sum** of every unreachable entry's timeout.
- `updateServers()` only ever *added*. There was no removal path anywhere in
  `client-ssl.cpp`: entries migrated between the live and pending-reconnect
  lists and never left either. A server deactivated in `config_serverconfig`
  dropped out of the broadcast list, but every client that had ever seen it kept
  it — and kept paying a connect attempt for it every backoff interval, for the
  process lifetime.

The shipped `examples/fake_client.cpp` drives all of it from one thread, and the
README names that example as the starting point for a real client. So the
aircraft's telemetry cadence and its reconnection latency were both set by the
*slowest* server rather than the fastest, and the set of slow servers only ever
grew.

Command *reception* was never affected — commands arrive on each connection's
own recv thread — which is the only reason this was a telemetry-availability
and reconnect-latency defect rather than a command-delivery one.

## Decision

**Give each `client_ssl::fss_server` its own outbound worker**, mirroring the
server-side per-client worker. `sendMsgAll()` and `attemptReconnect()` are now
non-blocking: they schedule, and each server's worker performs its own blocking
I/O. One wedged server stalls only its own thread.

**Telemetry is loss-tolerant, so the queue is bounded and drops the oldest.**
Under sustained backlog a wedged server sheds its stalest report and keeps the
freshest — telemetry only gets less useful with age. The cumulative loss is
available from `getDroppedSends()` and the first drop of each backlog episode
is logged.

**A learned server carries a last-seen time and is dropped after a window.**
Servers broadcast their list every 15 s; four missed rounds (60 s) is the
default before a learned entry is expired, with a WARN naming it. The number
that can be learned is capped, with one WARN when the cap first refuses one.
Config-file servers are exempt from both.

## Why C8 survives the fan-out becoming concurrent

The todo/12 **C8 invariant** — a single `fss_message` instance must never be
sent on two connections at once, because `sendMsg()` stamps the per-connection
sequence id *into the message* — used to hold for free, purely because the loop
was serial. Parallelising it had to re-establish the invariant, not assume it.

It is re-established the way todo/55 already did for server-side broadcasts:
`sendMsgAll()` packs the message **once**, and hands every worker the same
`shared_ptr<const buf_len>`. `fss_connection::sendPacked()` copies those bytes
and stamps only its own copy. No `fss_message` instance is shared, and the
`const` in the type enforces that the shared frame is never mutated. This is
reuse of already-published machinery, not new mechanism.

The consequence worth stating: **`sendMsgAll()` cannot carry a message that
needs the mutating path.** `sendPacked()` refuses `message_type_smm_settings`
outright (it would skip the decision-43 `wipeSecure` scrub), and the client
never sends one. The per-connection sends that *do* mutate — `sendIdentify()`,
`sendVersion()`, the RTT reply — deliberately stay inline on the recv thread,
where a stall can only affect the one connection it belongs to.

## Why a reconnect is harvested by a flag, not by `connected()`

The worker does the dial; the client's next `attemptReconnect()` pass promotes
the server into the live list. Every list mutation stays on the thread that owns
`servers_lock`, so the worker never reaches back into the client.

The worker publishes success only **after `reconnect()` has returned true** —
that is, after the version and identity handshake has been sent. Harvesting on
`connected()` instead would expose the window inside `reconnect()` between
installing the connection and sending that handshake, during which
`sendMsgAll()` could put telemetry ahead of the mandatory version message.
Setting the flag afterwards preserves the previous ordering exactly.

`serverRequiresReconnect()` discards a stale success for a server that is not in
the live list: that is a connection which died between the dial finishing and
the harvest, and promoting it would park a dead connection where nothing would
flag it — a fresh connection starts with liveness disarmed, so
`isServerTimedOut()` would never fire.

## Why expiry is time-based, and driven from `attemptReconnect()`

**Time, not rounds.** The obvious implementation is a per-server counter of
consecutive lists that omitted the entry. It is wrong in a way that matters: the
number of lists a client sees per unit time depends on how many servers it is
connected to, since each broadcasts its own. todo/70 is already a standing
complaint about the one class in the tree that counts ticks instead of reading a
clock; this is new timekeeping and should not repeat it. The clock is injectable
for the same reason every other one in the tree is — so the timing is testable.

**Teardown deferred.** `updateServers()` runs on a recv thread, and the server
being expired can be the very one whose list this is. Disconnecting it there
would have that thread join itself — survivable, because
`fss_connection::disconnect()` detaches in that case rather than deadlocking,
but only by leaking the thread. So `updateServers()` removes the entry from both
lists into a holding list, and `attemptReconnect()` — which already owns
connection lifecycle, on a thread that never holds `servers_lock` across a
blocking call — drains and disconnects it.

**Config-file servers are never expired** and do not count against the cap. They
are the operator's declared intent: a client that lost contact during a server
restart must not prune away the only address it could come back on.
Server-side, a failed read of `config_serverconfig` returns `nullopt` and the
poller keeps its previous cache, so a read outage cannot broadcast an empty list
in the first place (decision 24) — but the client must not depend on that being
true forever, and one empty round expires nothing regardless.

## Alternative deliberately not taken: document the constraint instead

The cheap option was to state in `fss-client-ssl.hpp` and the README that
`sendMsgAll()` and `attemptReconnect()` block for the sum of per-server
timeouts, and leave each integrator to thread it themselves. It is honest, and
it costs no ABI break.

It was rejected for the same reason todo/21 rejected it on the server side: it
pushes an identical hazard onto every consumer and guarantees each solves it
differently — or, more likely, discovers it in flight. cap-fmu is the first
consumer; there will be others.

## Alternative deliberately not taken: just bound the damage

Dropping the client's default `TCP_USER_TIMEOUT` well below 30 s and
round-robining one reconnect per pass would have reduced the worst case in a few
lines with no ABI break. It was rejected because it does not remove the
coupling: a healthy server's telemetry still waits behind an unhealthy one, just
for less long. The tighter timeout remains available per decision 26 and is
complementary.

## Known, bounded cost

A `disconnect()` that lands while a worker is inside a blocking
`connect()`/handshake waits for that attempt to finish. That is bounded by the
connect and handshake timeouts (≈10 s worst case) — the same bound the caller's
thread paid for the same work before this change — but it means shutdown is not
instantaneous when a configured server is unreachable. A subclass that overrides
`reconnect_to()` with something that can block *indefinitely* must make sure
`disconnect()` can release it.

Thread count rises to one worker per server on the aircraft. At the handful of
servers a real deployment configures this is nothing; it is noted here against
decision 23's thread-count concern.

## Where this is pinned

`tests/client_fanout_test.cpp`: a wedged server does not stall telemetry to a
healthy one; a fan-out stamps each connection's own id (the C8 case); a
backlogged queue drops oldest and counts the loss; an unreachable server does
not delay reconnection to a healthy one; repeated `attemptReconnect()` does not
pile up dials. `tests/client_server_list_test.cpp`: expiry after the window,
config servers exempt, a refreshed sighting resets the window, one empty round
expires nothing, expiry disabled by 0, the cap, and the config keys.
`e2e/test_server_list.py` covers the removal direction end to end against a real
server and database — the suite previously only covered learning a server.
