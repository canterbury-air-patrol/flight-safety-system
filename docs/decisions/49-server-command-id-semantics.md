# 49 — The FMU command identifier is the server's DB row id, scoped per server

**Decided** 2026-07-17. Shipped in 1.2.0. Wire field:
`fss_message_asset_command::server_command_id`, gated by
`FSS_FEATURE_SERVER_COMMAND_ID` (0x8).

## Context

cap-fmu (`todo/86` in that repo — the FMU-side dedup of redelivered commands)
needed to tell two cases apart when it receives an asset command:

- a *redelivery* of an operator action it has already actioned (the resend
  window, a reconnect identify, a server bounce), which must not be actioned
  twice; versus
- a genuine *operator retry* — the operator pressed the button again — which
  must be actioned, even when the command, payload and timestamp are byte-for-
  byte identical to the previous one.

Nothing on the wire distinguished them, so the FMU could swallow a real retry.

## Decision

The identifier is the **server's command DB row id** (`assets_assetcommand.id`,
a plain auto-increment primary key — no change required on the web side), and
it is **scoped per server**.

The contract the receiver may rely on is recorded in full on the field itself in
`src/fss-transport.hpp`; in summary:

1. Within one connection, the same id means the same operator action; a new id
   means a new action.
2. Ids must **never** be compared across connections.
3. A deliberate operator retry inserts a new row in *every* server's database,
   so it arrives as a fresh id on every connection — this is the property that
   closes the swallowed-retry hole.
4. `0` means "not reported" (legacy peer, or the capability was not negotiated).
5. The id is the row primary key — remembered forever — and the server only ever
   dispatches the latest row per asset, so an id can reappear only as a
   redelivery of that same action.

## Deliberate deviation from the original request

The filing asked for an identifier that is **identical across all connections**
for one operator push. That was not built, and should not be retrofitted
without revisiting this record.

Each server owns its own database. There is no shared id space and no
coordination channel between servers, so a cross-connection-stable identifier
would require inventing one (a shared sequence service, or a synthetic
content hash). What cap-fmu actually needs is retry-vs-redundant
distinguishability, and per-server ids deliver exactly that via property 3
above, at zero infrastructure cost.

## Not to be conflated with `dispatch_id`

`dispatch_id` is the per-connection message header id used for command-ack
correlation. It is scoped to a *single delivery* and is stable across resends of
that delivery. `server_command_id` is scoped to the *operator action* and
survives reconnects. They are different values with different lifetimes.

## Implementation trap, found during the work

The optional trailing read in `unpackData` must come **after**
`assign_coordinates`. A failed optional read latches the `BufferReader`
not-ok, and the truncation judgement must cover mandatory fields only —
reading the optional tail first makes every legacy GOTO frame decode to NaN
coordinates.

## Where this is pinned

Transport round-trip, legacy-frame and byte-layout tests; session tests for both
dialects; `e2e/test_command_identity.py` (delivered id equals the DB row id, and
an identical-payload retry row arrives under a fresh id); the server-bounce
test also pins that a redelivery reuses the same id.

See also [51 — optional trailing wire fields are prefix-closed](51-optional-trailing-field-rule.md),
which governs this field.
