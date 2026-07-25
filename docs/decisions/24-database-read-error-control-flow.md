# 24 — Database reads report failure by status; only writes throw

**Decided** 2026-07-16. Shipped in 1.2.0.

## Context

`getActiveServers()` signalled a cut-short read by throwing `database_error`
across a thread boundary, and the throw was swallowed by the recv thread's
`exception_guard`. Control flow that depends on an exception unwinding through
callers that never see it is not control flow anyone can reason about.

## Decision

**Reads return a status; they never throw.** `IDatabase::getActiveServers()`
returns `std::optional<std::vector<fss_server_details>>`, where `nullopt` means
the read was cut short — a mid-cursor error, or a
[41 truncation](#related) — with `db.cpp` logging and returning `nullopt`
instead of throwing.

`getServersListMsg` / `build_server_list_msg` translate that into a **nullptr
message**, which is deliberately distinct from a non-null zero-server message
for a genuinely empty set. The poller updates the cached list *and* runs
`refreshSmmSettings()` only on a non-null build, exactly matching the old
unwind-skips-both behaviour; the identify path skips the server-list send with a
WARN, where the old throw was swallowed with the send equally skipped. Both
production paths' observable behaviour is unchanged.

The filing named two callers. There turned out to be three — the poller, identify
on the recv thread, and both of those via `getServersListMsg`.

## `database_error` stays, for writes

The [34/45/47](34-45-47-db-failsafe-latch.md) write wrappers still throw
`database_error` into `db_write_queue`'s catch. That is a single-thread-local
idiom the fail-safe counts on: throw and catch are in the same thread, a few
frames apart. The exception's doc comment now states that reads never throw.

`getAssetId()` follows the same convention (todo/60): it returns
`std::optional<uint64_t>` where `nullopt` means the read failed and `0` means
definitively unknown, and the two identify call sites log those two cases
differently — ERROR "asset lookup failed (DB read error)" versus the existing
genuine-miss wording. The non-aircraft branch treats `nullopt` as "cannot prove
this CN is not an aircraft's" and rejects, rather than claiming it "belongs to a
known aircraft".

## Related

todo/41: `db_active_fss_servers_get` also fails the read when
`sqlca.sqlwarn[0] == 'W'`, so a truncation-triggered `WHENEVER SQLWARNING DO
BREAK` exit reports failure rather than shipping a partial active-server list as
if it were complete.
