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

**Recovery.** The write queue draining to zero, more than
`db_write_failure_recovery_grace_secs` of quiet, *and* a successful health probe
lower the gate. The probe condition was added 2026-08-02; see below for what it
replaced and why.

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

### Recovery needs positive evidence (revised 2026-08-02, todo/78)

As shipped, recovery was "the write queue has drained *and* nothing has failed
for the grace window". Both are statements about the **absence** of failure, and
the trip's own action removes everything that could produce one: `disconnectAll()`
severs every session and the admission gate refuses new ones, so no telemetry
arrives, nothing is queued, nothing fails. Drained-and-quiet was therefore
satisfied **by construction** about `db_write_failure_recovery_grace_secs` after
every trip, whatever the database was doing. "Readmitted traffic is itself the
health probe" — the sentence this section replaces — describes a probe whose
result arrives only after the fleet has already been let back in.

Path M m05 showed what that costs. A 200 MB Postgres data directory was filled
until the instance hit a `PANIC` on its WAL, and its crash recovery could not write
either, so the instance was permanently gone:

```
08:49:21 DB fail-safe tripped (sustained DB write failures); severed 1 connection(s)
08:49:28 Refusing new client: DB fail-safe is degraded
08:49:37 DB fail-safe recovered: write queue drained and quiet for 15s; accepting sessions again
```

Sixteen seconds. Against a permanently dead database that is a ~20 s cycle
forever, and each cycle takes a connected aircraft out of and back into its
comms-loss failsafe — the opposite of the one clean latched event 47 exists to
produce.

**Decision.** Recovery now also requires a **probe write** to have succeeded
since the trip:

```
recovered  ⟺  pending_writes == 0
           ∧  no new write failure, command drop or probe failure for the grace window
           ∧  a probe success observed on this very tick
           ∧  probe_successes != probe_successes_at_trip
```

The probe **gates** the quiet window, it does not replace it. Since the probe
runs on the caller's per-second tick and any probe failure restarts the quiet
window, recovery in the default configuration needs roughly fifteen *consecutive*
successful probes, not one lucky one.

**The evidence must be fresh, and freshness is edge-triggered.** "A probe has
succeeded at some point since the trip" is not enough, because a probe that
**hangs** — a partition, a failover, a frozen host — returns neither answer: no
success, no failure, no counter movement at all, and `requestProbe()` coalesces
so nothing queues up behind it either. A failing probe holds the latch by
restarting the quiet window; a hanging one can only be caught by the absence of
*new* evidence.

A time-boxed version of that test ("a success within the last
`recovery_grace_ms`") sounds equivalent and is not, which is worth recording
because it is the obvious formulation. The quiet window runs from the trip,
which is itself an event, so recovery is first possible one tick after
`trip + grace` — and a success landing one tick *after* the trip is at that
moment almost exactly `grace` old, i.e. still inside such a window. With the
defaults: trip at t=1 s, one success at t=2 s, database hangs, quiet satisfied
from t=16.001 s, success age 14.001 s < 15 s → recovered, on the strength of a
single answer given fourteen seconds earlier. Requiring the success on the
recovering tick has no constant in it and no boundary to land on.
`tests/server_failsafe_test.cpp` pins that arithmetic directly.

Cost on a healthy database: none. The caller requests a probe on every degraded
tick and the write queue is empty while degraded, so every tick observes the
previous tick's success and recovery lands on the tick it always did — confirmed
against the e2e timings, which did not move. A database answering *more slowly
than the tick period* delays recovery by at most one probe round trip; while it
is answering that slowly, holding the gate up is the safer error.

The probe statement, on the **write** connection, inside a transaction that is
rolled back:

```sql
INSERT INTO assets_assetrtt (asset_id, rtt, timestamp)
    SELECT id, 0, NOW() FROM assets_asset ORDER BY id LIMIT 1;
```

- Both tables are already in `required_columns[]`, so this adds no table, no
  column, no fss-web migration and no change to the minimum migration floor.
- The `SELECT`-driven asset id satisfies the foreign key without the server
  having to remember an id across a severance that has just discarded every
  session.
- A zero-row result (`sqlca.sqlerrd[2] == 0`, i.e. an empty `assets_asset`) is
  **inconclusive**, not success: a zero-row INSERT fires no row trigger, extends
  no heap page and writes no tuple, so it proves nothing. It is counted and
  logged separately, and the fail-safe stays degraded on it.

`db_failsafe` remains a pure state machine that performs no I/O: the probe runs
on the `db_write_queue` worker thread — which already owns the write connection
and already blocks on it by design — and its outcome reaches `tick()` as
cumulative counters, exactly like `write_failure_count()`. The main loop only
sets a flag (`requestProbe()`), so [46](46-no-statement-timeout.md)'s
no-synchronous-DB-work-on-the-main-loop contract is untouched. Probes are asked
for only while degraded (`wantsProbe()`), so healthy operation pays one bool
read per second.

Operator visibility came with it, because the fix makes silence the *correct*
outcome against a dead database and silence is exactly what an operator cannot
act on: a rate-limited (60 s) ERROR while degraded naming the elapsed time, the
probe failure and inconclusive counts and the last probe error, and a recovery
line that states the evidence ("drained, quiet for 15s, **and a probe write
succeeded**").

**Accepted residual — the probe does not prove durability.** A rolled-back
INSERT exercises parse, plan, permissions, triggers, foreign keys, heap-page
extension and WAL *generation*, but an aborted transaction never forces a WAL
fsync. A database that can buffer a write but not flush it can therefore pass
this probe and still fail a real commit. This is accepted: the failure modes
that actually produced this defect (a dead instance, a full disk, a write-only
fault) all fail the probe as it stands, and the upgrade path is named below.

**Rejected, and why:**

- **`SELECT 1` (or any read).** Disproved by an existing test:
  `e2e/test_db_write_only_failure.py` installs `BEFORE INSERT` triggers that
  raise on the four telemetry tables — `assets_assetrtt` among them, so the
  probe's own target is faulted by that test — and reads stay perfectly healthy
  throughout. A read-only probe would report health while every write failed,
  which is the exact fault class the fail-safe exists for. (`db_ping`'s
  `SELECT 1` remains right for what it does: connection liveness for
  `tryReconnectIfNeeded`.)
- **Committing the probe row.** It would fabricate an `rtt = 0` telemetry
  sample, attributed to a real aircraft that is disconnected, in a table the
  operator reads. This service exists to produce a truthful audit record; a
  health check must not write fiction into it. ECPG ends an open transaction by
  COMMITting it when `AUTOCOMMIT` is switched back on, so `db_probe_write`
  issues an explicit `ROLLBACK` first and leaves the connection transaction-idle.
- **A dedicated probe table.** The clean answer, and the named upgrade path for
  the durability residual above — a table FSS owns, written and committed. It is
  not available today: every table in this schema belongs to fss-web. Adding one
  to `required_columns[]` would turn a health-check improvement into a
  fleet-wide startup refusal on any deployment that has not run the new
  migration (decision 73), and leaving it out of `required_columns[]` means the
  startup check cannot guarantee the probe works at all. Revisit when fss-web
  next takes a migration for FSS's sake.
- **Making the probe a `db_write_task`.** A queued probe makes
  `pending_count()` non-zero, which the drain check reads as "not drained".
  Recovery would become unreachable and the fleet would stay severed forever —
  worse than the defect being fixed. The probe is a separate flag with a
  separate function, and `pending_count()` is untouched.
- **Probing from the main loop.** A synchronous round trip on the loop that
  dispatches TERM and DISARM, which is precisely what decision 46 removed.
- **Probing from the `command_poller`.** It owns the *read* connection, so it
  would prove the wrong path — see the `SELECT 1` argument above.
- **A dedicated probe thread.** Needs either a third connection or contention on
  `write_lock` with the writer, for no benefit over the worker that already owns
  the connection and is already allowed to block.
- **A `statement_timeout` on the probe.** Decision 46 rejected it globally and
  the same reasoning applies here: `e2e/test_slow_db_does_not_stall.py` holds an
  `EXCLUSIVE` lock the writer is expected to ride out, and that must present to
  the probe as "no answer yet", not as a probe failure. A lock wait blocks the
  probe; it cannot manufacture one. The stall bound stays `PGTCPUSERTIMEOUT`
  (25 s) at the socket.

### The recovery grace backs off per re-trip (revised 2026-08-05, todo/81)

The probe answers "does a write work **right now**?", and that is the whole
question against a database which is dead or persistently faulted: it never
answers yes, so the latch holds. An *intermittently* faulted one answers yes
honestly, over and over. A fault whose good periods outlast the grace window
gives: queue drained, window quiet, probe succeeds, fleet readmitted, real
telemetry resumes, fault returns, incident ages past
`db_write_failure_disconnect_secs`, trip — round again. Every readmission is
backed by evidence that was true when it was taken. The cycle is much slower
than the pre-78 one, a full incident timescale rather than a fixed ~20 s timer,
but an aircraft is still being moved in and out of its comms-loss state by a
fault the server has already seen.

The missing question is not "does a write work now?" but "**has this database
been reliable long enough to trust it with the fleet?**", which is about a
history rather than a moment.

**Decision.** The recovery grace is multiplied by the length of the current run
of trips, capped:

```
effective grace  =  db_write_failure_recovery_grace_secs
                 ×  min(trips in the current run, db_write_failure_recovery_backoff_max)
```

A trip within an hour of the previous one continues that run; a trip after a
quiet hour starts a new one at 1. At the defaults (15 s grace, cap 8) a first
trip holds the admission gate for 15 s and an eighth within the hour holds it
for two minutes.

- **The first trip of a run is untouched**, so a one-off incident — the common
  case, and the one the field reports are about — behaves exactly as it did.
  What grows is the cost of a fault that keeps coming back, which is what makes
  a flap a flap.
- **The cap is required, not optional.** An unbounded multiplier would
  eventually refuse a fleet against a database that had genuinely recovered:
  the fail-safe's own failure mode, arrived at from the cautious side. `1`
  disables the back-off and restores the todo/78 behaviour exactly; `0` is read
  as `1` rather than as a grace of zero, which would readmit the fleet on the
  first quiet tick.
- **Only the recovery role of the grace is backed off.** The same config value
  also decides how long failures may pause and still chain into one incident.
  Stretching that would make each successive trip *later* as well as longer,
  i.e. leave the fleet writing into a database already known to be failing.
  A unit case pins the separation.
- **The run count doubles as the has-tripped-before flag**, for the same reason
  `incident_active` exists (todo/70): `last_trip_ms == 0` is a legitimate
  reading on a clock that has just started, so a zero timestamp cannot mean
  "never".
- **The still-degraded and recovered log lines report the effective window**,
  not the configured one. After a repeat trip the configured value would tell an
  operator watching a flapping database to expect readmission several multiples
  of the hold too early.
- **The one-hour window is fixed rather than configurable.** It only has to be
  long enough that a fault returning after one hold is judged the same fault,
  and the longest hold the cap allows is two orders of magnitude below it. One
  configuration field for the cap is the whole surface this adds.

**Rejected: K consecutive probe successes** (option 1 in the item). The grace
window already delivers an approximation of it for free — any probe failure
restarts the window, so recovery in the default configuration already implies
roughly fifteen consecutive successes. The real gap was never the count within
an episode; it was that the count restarts from zero after each recovery no
matter how many times the same fault has come back. K-consecutive answers the
within-episode question that was already answered, and leaves the across-episode
one open.

`e2e/test_db_failsafe_backoff.py` drives the flap end to end: install the fault,
let it trip, heal the database, watch the gate come down, let the fault return,
and assert the second episode holds for twice as long — measured from the moment
the database became healthy again, so the number is the window and not the
duration of the fault. The negative case was run: with the cap forced to 1 the
second episode recovers on the configured 15 s window and the test fails.

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
