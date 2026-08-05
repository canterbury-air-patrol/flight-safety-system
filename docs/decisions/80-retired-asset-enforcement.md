# 80 — Enforcing fss-web asset retirement

Status: implemented, shipped in 1.3.0. Requires fss-web `assets` migration 0013 or later, which is
already below the deployed floor (0016, decision 76) — so this raises no minimum
and needs no deployment coordination beyond the note at the end.

## The problem

fss-web's MODEL-02 added reversible retirement: `assets_asset.retired_at`
(nullable, indexed), plus an `on_delete=PROTECT` on `AssetCommand.asset` so a
command-bearing asset can no longer be deleted at all. Retirement is meant to
*replace* deletion — the identity and its audit history stay, and clearing the
column returns the asset to service.

That removes a retired asset from fss-web's active APIs and blocks new web
commands. It did nothing to FSS, which never read the column. A retired
aircraft could still identify, still write telemetry, and still be handed the
newest command row.

Filtering the identify query alone does not close it. A session resolves its
`asset_id` once, caches it in `cached_asset_id`, and never repeats the lookup;
an aircraft that was active when it connected keeps flying for as long as it
stays connected. Retirement that only bites at the next reconnect is not a
fleet-facing control — it is a UI state that an already-airborne aircraft
ignores. Both halves had to be covered.

## The identify half

`db_get_asset_id`'s `SELECT id ... WHERE name = :asset_name` takes
`AND retired_at IS NULL`.

No new return convention was needed. todo/69's `error_out` split already lives
in this function, and a retired row falls out as the existing *unknown asset*
path: an engaged 0, the same answer an unregistered CN gets. That is the right
place for it — the database answered successfully, and its answer is "no
flyable asset by that name". A read failure remains the only `nullopt`.

Collapsing the three cases either way is the bug. As an error, retirement would
trip the identify path's read-outage handling and read as a DB incident. As a
hit, a retired aircraft flies.

## The live-session half: which query

The open question in `todo/80` was whether to extend `getCommands()` to carry
retirement alongside the command, or add a second batched query. The filing
guessed the former was "one round trip" and therefore cheaper.

It is not available. `db_asset_commands_get` selects
`FROM assets_assetcommand WHERE AC.asset_id IN (...)`, so **a retired asset with
no pending command produces no row at all** — there is nothing for an extra
column to ride on. Carrying retirement there would mean restructuring to
`assets_asset LEFT JOIN assets_assetcommand`, which also breaks the contract
todo/69 established for that result: "absent from the map" would stop meaning
"no pending command".

So: a second query, `db_asset_retired_get`.

```sql
SELECT id FROM assets_asset WHERE retired_at IS NOT NULL AND id IN (...)
```

It returns a subset of the ids asked about — normally empty — and reuses the
`asset_ids` vector `pollCommands()` already builds. The `IN` filter is kept
rather than selecting every retired asset, so the result stays bounded by the
connected fleet instead of growing with every airframe ever retired.

**A partial read is discarded, unlike `getCommands`.** Both reads use the
`error_out` convention, but they fail in opposite directions. A command missing
from a truncated list means a command is not dispatched this tick; a retired
asset missing from a truncated list reads as *active*, and the aircraft keeps
flying. `nullopt` is therefore not an empty set: an empty set is the positive
claim that every asset in the batch is active. On a failed read the server
severs nobody and retries.

## The live-session half: which thread

Detection and severing run on different threads, and that split is the point.

Detection is a database read, so it may only happen on the command poller — the
main loop never performs a synchronous DB read. Severing is `disconnect()`,
which blocks on socket I/O and joins the recv thread. Running that on the poller
would let one black-holed peer stall command polling for the entire fleet, which
is the hazard todo/46 moved `db_ping` off that thread to avoid.

So `pollRetiredAssets()` records who to sever and returns; the main loop drains
the list through `disconnectRetiredClients()`, where `disconnectRevokedClients`
and the fail-safe's `disconnectAll` already do the same work. The hand-off list
is deduplicated, because the poller re-observes a retirement on every pass until
the drain happens — severing is idempotent, but a second severance warning in
the log reads to an operator as a second session that never existed.

Severing reuses the `disconnect()` + `clientDisconnected()` pair rather than
anything new. That matters for reactivation: `clientDisconnected()` is what
releases the `asset_owners` claim (todo/44), and a severing path that skipped it
would leave the id unclaimable, so the reactivated aircraft would be rejected as
a duplicate of a session that no longer exists — until a server restart.

## The bound

The poller checks once a second (alongside `tryReconnectIfNeeded`), not on every
100 ms tick: retirement is an administrative act, and polling at 100 ms would
double the poller's query rate to chase an event that happens a handful of times
a year. The main loop drains on every tick.

**Worst case from retirement to severance is therefore ~1.1 s** — up to one
second before the poller observes it, plus up to 100 ms before the main loop
severs — plus the teardown itself.

Within that window the session is still live and its telemetry is still
accepted. That is a deliberate, bounded exposure, not an oversight: the
alternative is a synchronous read on the session hot path, which item 3 of the
filing ruled out for good reason. Writes already accepted before the observation
drain under the existing writer contract; nothing new is accepted after it.

## Startup gate

`assets_asset.retired_at` is in `required_columns[]`, making it the second
startup gate after migration 0008's ack columns (decision 73). A release that
enforces retirement refuses to start against a schema that cannot express it,
and names the column.

This is the intended behaviour and it has an operational consequence worth
stating plainly: **an operator who upgrades FSS ahead of fss-web gets a server
that will not start, not a server that silently ignores retirement.** The
ordering was previously documented; now it is enforced.

In practice this gate is already satisfied — the deployed floor is migration
0016 — so it is insurance against a rollback of fss-web, not a new coordination
requirement.

## What is not covered here

- **Notification instead of polling.** fss-web's migration 0012 installs a
  `pg_notify` trigger, but it is `AFTER INSERT ON assets_assetcommand` only;
  nothing fires on an `assets_asset` UPDATE. Taking that route would need a new
  fss-web migration *and* a libpq listener FSS does not have — the same
  dependency todo/23 phase 2 carries. Worth revisiting only if that listener is
  built for other reasons.
- **Telling the aircraft why.** A severed session gets a disconnect, not a
  reason code. The aircraft treats it as any other comms loss. Distinguishing
  "retired" from "server gone" on the wire is a protocol change (todo/17's
  territory) and no operational need for it has been stated.
