# 73 — The server verifies the database schema at startup and refuses to run without it

**Decided** 2026-07-28. Unreleased.

## Context

The tables FSS reads and writes are not FSS's. They belong to
[fss-web](https://github.com/canterbury-air-patrol/flight-safety-system-web/), a
separate repository, and are created by its Django migrations. The
command-ack columns on `assets_assetcommand` — `dispatch_id`, `ack_state`,
`ack_timestamp`, `ack_superseded_by` — exist because *this* project added the
command-ack feature, so they arrived in an fss-web migration written for FSS's
benefit.

Nothing verified that the database FSS was pointed at actually had them, and the
failure mode of finding out at runtime is the worst one in the tree:

1. The server starts cleanly. Connecting proves nothing about the schema.
2. Every aircraft connects and identifies. Telemetry writes touch none of the
   ack columns, so the deployment looks healthy.
3. The first command dispatch calls `recordCommandDispatch`, which throws
   `database_error`.
4. `db_write_queue` counts sustained write failures and the
   [34/45/47 fail-safe](34-45-47-db-failsafe-latch.md) latches after ~5 s:
   `disconnectAll()` plus the admission gate.
5. **Every aircraft is severed and none can reconnect.** The condition never
   clears, because the schema is still wrong. Per todo/68 the dispatch write
   also recurs every 10 s per aircraft, so there is no quiet period in which the
   mismatch stays latent.

A missing migration in another repository therefore presented as a total,
self-sustaining fleet outage, minutes after an apparently successful deploy.

The e2e suite could not catch it: it provisions from `e2e/schema/001_init.sql`,
a hand-written mirror of fss-web's migrations with nothing verifying the
correspondence, so the suite passes against the schema FSS expects rather than
the one that will be there.

## Decision

`db_schema_check()` runs once at startup, on the read connection, immediately
after `isConnected()` and before the write queue exists or the listener binds.
It anti-joins every column the statements in `server-db.pgc` read or write —
listed in `required_columns[]` in that same file — against
`information_schema.columns`, in one round-trip, and also checks that the
PostGIS extension every position read and write depends on is installed. The
server refuses to start if anything required is missing, naming it.

All missing columns are logged before the refusal, so a single restart tells the
operator the whole gap rather than the first item of several.

`required_columns[]` is deliberately part of the statements above it: adding a
column to a query means adding it to that list. This is a second thing that can
drift, knowingly accepted, because it drifts *loudly* — the server stops
starting, at deploy time, on a developer's machine, naming what is wrong. The
positive case in `db_connection_test.cpp` fires the moment the list runs ahead
of the schema the suite is provisioned with.

### A missing column is fatal; a too-wide column is a warning

Five columns are fetched into fixed C buffers (`assets_assetcommand.command`,
`config_smmconfig.address`, `config_serverconfig.address`,
`config_assetconfig.smm_login`, `config_assetconfig.smm_password`). ECPG fills
such a buffer to capacity with no room for a terminator, so a value at or beyond
the buffer size is truncated, and `server-db.pgc`'s truncation guards then drop
the row — silently, from the caller's point of view.

The check reports a column declared wide enough for that to happen, but only as
a warning. Refusing to start over it would be the stricter reading of "exists
with a compatible type", and it is the wrong trade: widening a `VARCHAR` is a
routine fss-web migration that is harmless until something actually stores an
over-long value, and turning it into a refusal would produce exactly the
fleet-wide outage this check exists to prevent. A warning surfaces the drift
without betting availability on it.

### There is no way to skip the check

Considered and rejected: an optional `verify_schema` key in `server.json`,
defaulting true. A skip flag is set during an incident and never unset, which
restores precisely the failure mode above with the added irony of a log line
saying it was disabled on purpose. The check has no false positives to escape
from — it asserts only that columns the server unconditionally uses exist.

### A check that cannot be completed is also a refusal

If the check query itself fails, or an allocation failure would have dropped a
problem from the list, `error_out` is set and the server refuses. An incomplete
check must never read as a clean one, and a database that has just connected but
cannot be introspected is a broken deployment; the supervisor's restart is the
right response.

## What this does not close

todo/73 offered three fixes; this is option 3, and it is the only one that
protects a *production* deployment, which is where the risk actually lives.
The other two remain **deferred, not rejected** — they close the different gap
that the e2e schema is an unverified copy of another repo's migrations:

1. **Provision e2e from fss-web's real migrations** (`manage.py migrate` against
   the test Postgres, with `001_init.sql` kept as an offline fallback). Catches a
   breaking migration on the day it lands rather than the day it deploys. Needs
   an fss-web image and cross-repo CI access this repository does not have today.
2. **Diff the e2e database against a checked-in `information_schema` dump** from
   a migrated fss-web. No runtime dependency, but still a hand-maintained mirror
   — one with a tripwire.

Both need scheduling with whoever owns fss-web's CI. Until then the deployment
coupling is stated in `docs/release-checklist.md` and the README rather than
enforced by a test.

## Verification

- `tests/db_connection_test.cpp` — the check accepts the provisioned schema, and
  refuses it with `ack_state` dropped (RAII-restored).
- `e2e/test_schema_check.py` — the process-level half: with `ack_state` dropped,
  `fss-server` exits non-zero, the log names the column, and no listener is left
  behind.
- Manually confirmed during implementation: the boundary is exact
  (`VARCHAR(15)` against a 16-byte buffer warns not at all, `VARCHAR(32)` warns
  and still starts), a dropped table reports each of its columns, and an empty
  database reports the missing PostGIS extension followed by all 40 columns.

## Amendment: the first deliberate change to the mirror (todo/76)

`gps_fix_valid` on `assets_assetposition` is the first column added to
`required_columns[]` since this check landed, and so the first test of the
process it implies. Three things had to move in one commit: the entry in
`required_columns[]`, the table definition in the hand-written mirror
`e2e/schema/001_init.sql`, and the stated migration floor in the README and
`docs/release-checklist.md` (0008 → 0016).

What enforces the pairing is the *positive* case in
`tests/db_connection_test.cpp` — "verifySchema accepts the provisioned schema".
Adding a required column without adding it to the mirror fails that test
immediately, which is the intended tripwire and is why the negative
column-dropped case is not the important one here.

The same commit made `position` nullable in the mirror. The check only tests
for a column's presence, so a nullability change is invisible to it: had the
mirror kept `NOT NULL`, the server would have started cleanly and then failed
every no-fix write at runtime. Both open options above (provisioning from
fss-web's real migrations, or diffing an `information_schema` dump) would have
caught that; the presence check alone cannot, and the e2e no-fix test is what
covers it today. See [decision 76](76-gps-fix-recording.md).
