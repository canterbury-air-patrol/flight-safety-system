# 48 — A terminal command ack is final *for its dispatch*

**Decided** 2026-07-16. Shipped in 1.2.0. Enforced in the query, in
`db_command_record_ack` (`src/server-db.pgc`).

## Context

Command acks are audit state: they record what the aircraft did with an
operator's command. A later ack arriving for a command that had already
reported a terminal outcome could overwrite that outcome, destroying the record
of what actually happened.

## Decision

The write guard is **"writable only while unsettled"**: `db_command_record_ack`
updates the row only when the stored `ack_state IS NULL OR = 0`. This subsumes
the earlier "received cannot regress" rule and refuses any second terminal
outcome.

To keep redelivery working, `db_command_set_dispatch_id` **reopens the ack
cycle**: when it stamps a new dispatch id it clears the three ack columns. The
ack columns therefore always describe the *latest dispatch*, not the row's
lifetime.

### What fences a stale ack (revised by [68](68-command-redelivery-and-ack-keying.md))

As decided here, the fence was the freshly minted dispatch id: an ack for the
new delivery could only match the row by carrying that id, and the id did not
exist on the row until the same statement that cleared the acks landed.

**That argument no longer holds, and is not what protects the row today.**
Decision 68 keys the ack on the command row's primary key rather than on the
stored `dispatch_id`, and stops writing a dispatch on every resend — so the ack
no longer reaches the row via the stored id at all, and the clearing statement
fires once per delivery rather than once per resend.

The replacement fence is narrower and stronger:

- The ack names the **row**, and finality is scoped to that row's current
  dispatch via the unchanged writable-set guard. There is no id to collide and
  no subselect to pick the wrong row.
- Cross-connection staleness is impossible because the translation is
  in-process: `fss_client` holds the `dispatch_id → command_dbid` map, a
  server-side session is built per accepted connection and never reused, and
  the map dies with the session. An ack arriving on a new connection for a
  delivery made on the old one resolves to nothing and is dropped.
- Within one connection, the ack cycle is reopened exactly once — at the first
  delivery of a given command — and every later frame for that command is a
  resend that writes nothing. So no clear can land between a delivery and its
  ack.

## Alternative deliberately not taken: blanket "terminal is final"

A terminal outcome that is final for the *row* was rejected because a
legitimate terminal-over-terminal exists and is pinned by
`e2e/test_server_restart.py`: a command redelivered after a server bounce must
settle back to `actioned` with a fresh `ack_timestamp`. Blanket finality would
leave that row stuck at its pre-bounce outcome forever.

Decision 68 does not change this. A server bounce (or an aircraft reconnect)
builds a new session, whose `last_dispatch_write_dbid` starts at zero, so the
first delivery on the new connection still records a dispatch, still reopens the
cycle, and the re-ack still lands.

## A refused write is not an error

A policy-refused ack matches zero rows, which is not an SQL error. That is
deliberate: if a refused write raised, a forged or duplicated ack could
masquerade as a database write failure and feed the
[34/45/47 fail-safe](34-45-47-db-failsafe-latch.md), letting a malicious or
buggy peer sever the fleet.

## When todo/17's system-health work lands

todo/17 item 2 introduces an accepted-but-not-transmitted outcome. Extend the
*writable set* — classify that outcome as non-terminal — rather than weakening
finality.

## Where this is pinned

Unit tests: a second terminal is refused for each of the three outcomes; the
conforming received→terminal flow is unaffected; redispatch reopens and
re-acks. The server-bounce e2e still passes against the guard.
