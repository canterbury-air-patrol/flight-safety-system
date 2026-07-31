# 34/45/47 — The database fail-safe is one latched degraded state

**Decided** 2026-07-14 (45 and 47 shipped together; supersedes the 2026-07-13
incident tracker from 34). Shipped in 1.2.0. Implementation: `db_failsafe` in
`src/server-failsafe.hpp`.

## Context

This is a safety service. If the server cannot record what it dispatched and
what the aircraft acknowledged, the audit trail it exists to produce is being
silently destroyed. Three findings converged on one mechanism:

- **34** — the ECPG write functions returned `void` and never checked
  `sqlca.sqlcode`, so a failed write got ECPG's default `sqlprint()` while the
  C++ wrapper returned as if it had succeeded. `write_failure_count()` stayed at
  0 through an entire run against a genuinely full disk.
- **45** — a lost command/ack write did not degrade the server at all.
- **47** — degradation that severs clients without also refusing their
  reconnects just flaps: sever, re-identify within a second, sever again, every
  ~7 s. That flap was captured live against the pre-fix binary.

## Decision

One state machine owning both triggers and the latch.

**Triggers.** A `command_dropped_count()` increase trips *immediately* — a
dropped dispatch or ack write is known-destroyed audit state, with no threshold
to age through. A write-*failure* incident trips only when a **new** failure
arrives while the incident is already `db_write_failure_disconnect_secs` old.

That second condition also fixed a defect in the shipped 34 tracker: a *single*
transient failure kept the incident active through the recovery grace and
tripped at the age threshold with no further failure — one blipped INSERT
severed the fleet 5 s later, despite the code's own comment claiming "a single
transient failure never trips this".

**On trip.** The main loop raises `server_clients`' admission gate
(`setDegraded(true)`) **before** `disconnectAll()`. Ordering is load-bearing:
the gate check shares `lock`'s critical section with list insertion, so no
connection can slip in between the severance snapshot and the gate. Refused
clients are severed unactivated.

**Recovery.** The write queue draining to zero *and* more than
`db_write_failure_recovery_grace_secs` of quiet lowers the gate; readmitted
traffic is itself the health probe. A persistent fault re-latches on the
incident timescale rather than flapping.

### Both windows are real time (revised 2026-07-31, todo/70)

As shipped, `db_failsafe` counted `tick()` calls and both thresholds were
expressed against that counter. The caller drove it from a loop whose period was
a 100 ms sleep plus the loop body's work, and which a signal could cut short, so
the windows above stretched under load and compressed under signal traffic —
load being correlated with the database being unwell, the trip ran late in
exactly the conditions it exists for.

`db_failsafe` now takes an injectable `IClock` and measures elapsed milliseconds.
No threshold, comparison or transition changed; what changed is that they are
now measured rather than counted, and can be asserted at the edge. The main loop
also runs its periodic tasks on deadlines rather than an iteration count, so the
other per-second work (`checkTimeouts()`, `sendRTTRequest()`) keeps honest time
too.

The config field `db_write_failure_disconnect_ticks` is renamed
`db_write_failure_disconnect_secs`, which is what it always meant; the old key
is still accepted with a deprecation warning.

## Alternatives deliberately not taken

- **A command-write journal with backpressure** — 45's own "preferred
  architecture". Minimum containment plus the unified gate meets the acceptance
  criteria, and a command-only queue overflow requires the entire
  default-10 000-deep queue to be command writes.
- **Exiting non-zero on trip.** Drain-plus-quiet defines an in-process recovery,
  so a restart is not the only safe path out of the degraded state. A safety
  service that can recover in place should.

## Notes from the verification work, worth keeping

- `REVOKE` is useless for simulating write failure in the e2e suite: the e2e
  server connects as a superuser, which bypasses grants. Use `BEFORE INSERT`
  triggers that `RAISE` on the telemetry tables instead —
  `e2e/test_db_write_only_failure.py` does exactly this and gets a genuine
  write-only failure.
- PostgreSQL does not degrade gracefully near WAL-segment exhaustion (~16 MB
  free): it PANICs, and because its own crash recovery also needs to write WAL,
  the whole instance exits rather than restarting. This is not a fixable test
  artifact, so `e2e/test_db_disk_full.py`'s recovery check uses a fresh
  container plus a fresh server and client.

## Related

[46 — no `statement_timeout`](46-no-statement-timeout.md) explains why a stalled
database must *not* be turned into write failures that feed this latch.
[48](48-command-ack-terminal-finality.md) explains why a policy-refused ack
matches zero rows instead of raising — so a forged ack cannot trip this.
