# 68 — Command acks name the row; redelivery ends at a terminal ack

**Decided** 2026-07-31. Shipped in 1.3.0. Implemented in `src/client_session.cpp`
(`sendCommand`, the `command_ack` handler) and `src/server-db.pgc`
(`db_command_record_ack`). Revises the fencing argument in
[48](48-command-ack-terminal-finality.md) and a factual claim in
[49](49-server-command-id-semantics.md).

## Context

Three facts composed into something nobody had written down:

1. Nothing retires a command row. Both command reads take the newest row for an
   asset with no filter on ack state, so the newest row is an asset's pending
   command forever.
2. The poller re-arms it every 100 ms.
3. `sendCommand()` re-dispatches on a 10 s timer.

So an operator issuing RTL once caused the aircraft to receive RTL every 10 s
for as long as it stayed connected. That much is defensible — `command_ack_noop`
exists precisely to describe "the command resolved to the state already
current", so the system treats the newest row as *commanded state*, not as an
event.

What followed from it was not. Every redelivery enqueued a
`command_dispatch_write`, and `db_command_set_dispatch_id` nulls `ack_state`,
`ack_timestamp` and `ack_superseded_by` to reopen the ack cycle. So the stored
outcome of a command was **destroyed and re-established every 10 seconds,
permanently**. An investigator reading `assets_assetcommand` saw the ack for the
most recent redelivery, never the aircraft's original response; an aircraft that
went quiet between a redelivery and its ack left the columns reading NULL,
indistinguishable from "never acked". The cost was ~18 protected
(non-evictable) writes per minute per aircraft, indefinitely, for a command
issued once — and the fail-safe trips unconditionally on the first dropped
command write.

The command/ack link is the audit state the
[34/45/47 fail-safe](34-45-47-db-failsafe-latch.md) exists to protect. The
healthy path was destroying it on a 10-second cycle.

## Decision

### 1. The ack names the command row, translated in-session

`fss_client` keeps a bounded FIFO of `dispatch_id → command_dbid`, populated on
every successful send. The `command_ack` handler resolves the wire's
`acked_command_id` through it before enqueueing, so `command_ack_write` carries
the row's primary key and `db_command_record_ack` is
`WHERE id = :command_dbid AND (ack_state IS NULL OR ack_state = 0)`.

This replaces a reconstruction: the query previously matched on
`(asset_id, dispatch_id)` and then took `ORDER BY timestamp DESC LIMIT 1`,
because the wire id is a per-connection counter that restarts at 0 and so is
unique to neither the asset nor the session. The dispatching session knows the
answer outright; reconstructing it in SQL was always an approximation.

Two consequences worth stating:

- An ack naming a dispatch this session never made is now **dropped with a
  WARN** instead of becoming a plausible-looking update. Nothing validated that
  before — the rate-limit comment on the `command_ack` arm said so explicitly,
  as the reason acks get their own token bucket. The bucket is still needed: a
  peer can re-ack a command it was legitimately sent, without limit.
- The map is empty on every new connection *by construction*, because a
  server-side session is built per accepted connection and never reused. There
  is no clearing step to remember.

### 2. A dispatch is recorded once per connection, not once per resend

`sendCommand()` enqueues `command_dispatch_write` only on the first delivery of
a command on a connection. The map insertion still happens on every send, so
every delivery stays ackable — only the write, and therefore the ack-column
clear, is suppressed.

`last_dispatch_write_dbid` is deliberately separate from `last_command_dbid`,
which `mark_handled` also sets on a permanent validation refusal: a command that
was never sent must not count as dispatched.

### 3. A terminal ack ends that command's redelivery

Once the aircraft reports `actioned`, `superseded`, `rejected` or `noop` — any
outcome but `received` — the session stops re-sending that command. A newer
command dispatches immediately regardless.

`received` does not count, and that is the same distinction
`db_command_record_ack`'s writable set draws: it says the frame arrived, not
that the aircraft acted on it.

## The invariant that makes (3) safe

**The stop is scoped to the connection, not to the command row.**

cap-fmu has no persistent storage. The server must therefore never treat an ack
from an earlier connection as evidence that the aircraft still holds the
command. It does not, because `terminally_acked_dbid` lives in the per-session
`fss_client` object and the server constructs a new one for every accepted
connection.

So for every way an aircraft can lose its commanded state — a process restart, a
power cycle, a link drop, a server bounce, a duplicate-identity eviction — the
recovery is the same and is automatic:

1. The old connection dies, and its session with it.
2. The aircraft reconnects as a new connection with a new session, where
   `terminally_acked_dbid` and `last_dispatch_write_dbid` are both 0.
3. Identify-time dispatch delivers the pending command immediately.
4. The 10 s resend loop runs until *that* connection produces its own terminal
   ack.
5. The fresh dispatch write reopens the ack cycle, so the re-ack lands — which
   is what `e2e/test_server_restart.py` pins from the server-bounce direction.

This is pinned, not merely argued: a unit case drives a second session for the
same asset and command row after the first acked terminally, and
`e2e/test_command_ack.py::test_restarted_client_is_dispatched_to_again` kills the
client process outright and requires a second dispatch and a re-ack with a fresh
`ack_timestamp`.

### The gap this leaves, named rather than hidden

If the **autopilot underneath cap-fmu** reboots while the FSS connection stays
up, there is no connection change, so the server has nothing to re-dispatch on
and cannot observe the event. Before this decision the 10-second redelivery
would have papered over it within 10 seconds.

That is cap-fmu's to handle, and is raised as their todo/108 — which also asks
whether the FMU relies on redelivery as a keep-alive *within* a connection. If it
does, a slow re-assert can be added on top of this; see the rejected option
below.

## What this does to decision 48

Decision 48's "no ordering race" argument was that the freshly minted dispatch
id fenced a stale ack, because it did not exist on the row until the statement
that cleared the acks landed. That premise is gone: the ack no longer reaches
the row via the stored id, and the clear fires once per delivery.

The replacement fence is narrower and stronger — the ack names the row, and
cross-connection staleness is impossible because the translating map dies with
the session. Decision 48 has been revised in place rather than superseded; its
terminal-finality rule and its "a refused write is not an error" rule are both
unchanged.

## Alternatives deliberately not taken

- **Carry `ack_state` on the batched command read** (todo/68 option 1's other
  route). It costs an ECPG change, two struct changes and a column on the hot
  poller query — and, decisively, it would make the redelivery stop *survive a
  reconnect*, which is exactly the property that must not hold.
- **A periodic re-assert after a terminal ack** — re-sending commanded state
  every few minutes as a keep-alive. Rejected for now because the only case it
  covers beyond the reconnect path is the autopilot-reboot case above, which the
  server cannot observe and cap-fmu can. It reintroduces churn to work around
  something at the wrong layer. Revisit if cap-fmu's answer to todo/108 is that
  they cannot re-apply.
- **A `server_command_id` on the ack wire** (per
  [51](51-optional-trailing-field-rule.md)'s prefix-closed rule). This would make
  the ack self-describing without the in-session map, but it needs cap-fmu
  agreement and FMU-side work first, and the map achieves the same correctness
  with no wire change.
- **An ack history table** (todo/68 option 2) — one row per ack, leaving
  `assets_assetcommand` carrying only the latest. Still worth having, but
  `assets_assetcommand` is owned by fss-web's migrations and since
  [73](73-startup-schema-verification.md) the server refuses to start against a
  schema it does not fully own, so FSS cannot add it unilaterally. Raised as
  fss-web's `MODEL-04`. After the three changes above the remaining loss is
  narrow — a reconnect redelivery reopens the cycle once — so it is a durability
  improvement rather than the fix.

## Where this is pinned

Unit: an ack for an un-dispatched id reaches no write; four resend windows
produce four deliveries and one dispatch write; an ack echoing a *resent*
frame's id still resolves; a terminal ack stops the resend and a `received` one
does not; a newer dbid dispatches regardless; a fresh session resends despite
the previous session's terminal ack.

Live DB: an ack updates only the row it names when two rows share a
`dispatch_id`.

E2e (`e2e/test_command_ack.py`): collisions across a seeded band of
`dispatch_id`s touch nothing; a killed and restarted client is dispatched to
again and re-acks; and a steady state spanning two resend windows shows the ack
columns never moving and exactly one dispatch logged — todo/68's acceptance
criterion, measured.
