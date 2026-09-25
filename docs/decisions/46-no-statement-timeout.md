# 46 — No `statement_timeout`; DB stalls are bounded at the socket

**Decided** 2026-07-15. Shipped in 1.2.0.

## Context

The main loop made blocking database calls — `tryReconnectIfNeeded()` issued a
live `SELECT 1` per connection — violating its own no-synchronous-read contract.
A wedged database therefore stopped command dispatch entirely.

## Decision

Two parts, neither of them a statement timeout.

1. **Move the work off the main loop.** `tryReconnectIfNeeded()` now runs on the
   `command_poller`, which already owns every database read: reconnect-before-
   read at the same 1 s cadence via the existing tick counter. A black-holed
   connection now degrades to "caches go stale", never to "commands stop
   dispatching".
2. **Bound in-flight stalls at the socket.** `PGTCPUSERTIMEOUT=25000` in
   `db_connect()`'s run-once env block puts in-flight death on the same 25 s
   clock as the keepalive envelope, so idle death and in-flight death agree. As
   an *intended* consequence, a sustained network partition makes writes fail
   after ~25 s and trips the
   [34/45/47 fail-safe](34-45-47-db-failsafe-latch.md) instead of hanging the
   writer forever.

## Alternative deliberately rejected: `statement_timeout`

Two reasons:

- It is **backend-enforced**, so it bounds neither black-hole variant. If the
  database host is frozen or partitioned, the backend that would have enforced
  the timeout is exactly the thing that is not running or not reachable.
- It would turn legitimate lock waits into fail-safe-feeding write failures.
  `e2e/test_slow_db_does_not_stall.py` deliberately rides out an `EXCLUSIVE`
  lock; under a statement timeout that becomes a fleet severance.

## Test deviation, and what it taught

The planned unit test (a blocking `db_ping` stub) was not buildable: the
loop/poller composition lives only in `main()` and is not linkable into
`all_test`. The property is pinned end-to-end instead —
`e2e/test_db_blackhole.py` freezes PostgreSQL with `docker pause`, the pure
black-hole case (the container kernel still ACKs, so no client-side option can
unwedge an in-flight query; only the thread move protects the loop) and counts
client-side `RCVD_RTT_REQ` lines. Verified against the pre-fix binary: 0 RTT
requests during a 25 s freeze — a 60 s probe also showed 0, the wedge is total —
versus ~24 on the fixed binary, 3 runs out of 3. The test also pins that the
fail-safe does **not** trip on a stall: frozen-DB writes hang, they do not fail.

Two findings from that verification worth remembering:

- Server RTT requests flow at ~1/s — a fresh request whenever none is
  outstanding. `rtt_retry_interval` only throttles *re-requests* while one is
  unanswered.
- The first version of that test opened its counting window *before* the freeze
  landed, letting in-flight sends mask a genuine wedge. The window now opens
  post-freeze plus a settle.

## Residual, documented and accepted

- Identify and the poller itself still stall behind a wedged connection's mutex
  — bounded at ~25 s in the partition case, unbounded for a frozen host. No
  client-side option can detect an ACKed-but-unanswered query.
- CRL checks still read the CRL on the main loop. Socket shutdown is immediate;
  session thread joins now run on the cleanup worker (see below).
- The network-partition e2e variant (a dedicated container on a user-defined
  docker network) that would exercise `PGTCPUSERTIMEOUT` and the fail-safe trip
  end to end was deferred, not done.

## Correction: independent connection health checks

The command poller now checks only the read connection. Write health checks
run on the write worker before a task or recovery probe, at most once a
second. Checking both from the poller reacquired the write mutex and blocked
new commands behind a telemetry lock wait, defeating the connection split.
The write-stall e2e test now observes a blocked INSERT in `pg_stat_activity`
before inserting a command, instead of assuming a fixed sleep establishes it.

## Correction: deferred session teardown

A timed-out identify can still be blocked on the database when removed from
the live client list. Joining its receive thread on the main loop therefore
reintroduced a global database stall through cleanup, even without a database
call on the loop itself.

Removal now shuts the socket down and stops outbound scheduling immediately.
A single cleanup worker retains removed sessions and performs their blocking
joins. Timeout, retirement, revocation, eviction, and fail-safe paths share
this removal path. Activation and final teardown are serialized so an
activation flushing queued messages cannot outlive or reinstall its handler
after teardown. A lookup returning after removal cannot reclaim an identity.

Shutdown still waits for callbacks to finish before destroying the client
registry or database. A permanently stalled DB can consequently delay graceful
process shutdown, but cannot stall fleet heartbeats during normal operation.
