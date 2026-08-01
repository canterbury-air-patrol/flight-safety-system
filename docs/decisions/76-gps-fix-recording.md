# 76 — Recording whether a reported position is GPS-backed

Status: implemented. Supersedes the transition-table shape sketched in
`todo/76`; fss-web chose to carry the state on the position row instead.

## The problem

An aircraft that loses its GPS fix does not go quiet. cap-fmu keeps reporting
at its normal cadence and marks the reports: it clears bit 0 (`VALID_COORDS`)
of the position report's MAVLink-style flags word, and sends the coordinates as
the wire no-fix sentinel (`INT32_MIN`, decoded back to `NaN`).

The server recognised the sentinel and discarded the report, incrementing a
counter and logging every hundredth. It never read the flags word at all. So a
GPS outage reached the operator as a position that stopped advancing — while
RTT kept the asset marked `connected` throughout. A MAVLink dropout, a stalled
position stream and a wedged FMU all look exactly the same. The four call for
different responses, and losing GPS is the one where a GOTO cannot be flown
accurately and an RTL cannot be relied on to navigate home.

The information was in the process. It just never left it.

## What is stored

fss-web owns the schema and chose to put the state on the position row
(migration 0016) rather than in a separate transition table:

- `assets_assetposition.gps_fix_valid BOOLEAN NOT NULL DEFAULT TRUE`
- `assets_assetposition.position` made nullable

Their API then serves the latest `gps_fix_valid = true` row as `position` (the
last trusted location) and the latest row overall as the `gps` block, with a
`position_estimate` when the newest report is a dead-reckoned one. That shape
decides ours: the row must exist per report, because the display ages the
latest row to judge whether the no-fix state is *current*. A transition table
would have stored two rows per outage but left nothing to age.

## The three cases

| Report | Stored | Broadcast |
| --- | --- | --- |
| Finite coordinates, fix valid | position, `gps_fix_valid = true` | yes |
| Finite coordinates, coords-valid bit clear | position, `gps_fix_valid = false` | yes |
| No coordinates (NaN sentinel) | NULL geometry, `gps_fix_valid = false` | **no** |
| Out of range or infinite | nothing | no |

A NaN is still never relayed: delivered to another aircraft it is a hazard, not
information. A dead-reckoned coordinate *is* relayed, flags word intact, so the
receiver applies its own policy.

An out-of-range coordinate is discarded and deliberately leaves **no** fix
record. It is a malformed report, not a report about the GPS; recording it as a
no-fix would make a buggy client read as an aircraft that had lost its fix.

NULL geometry rather than `(0, 0)`: Null Island is a legal coordinate that
renders as a real position. This is the same `NaN` ↔ `NULL` convention the
command read path already uses, carried through `position_write`, `IDatabase::
recordPosition` and ECPG indicator variables.

## Why a cleared flag never discards a position

A client can send finite coordinates with the coords-valid bit clear — that is
the estimator's dead-reckoned guess, which drifts but stays plausible-looking.
The obvious reading is to treat it like the NaN case and drop it.

It is stored instead. A safety system must not discard a real coordinate on the
strength of a metadata bit, and a drifting estimate that says so is strictly
more information than either storing it silently (the old bug) or storing
nothing. fss-web renders it as an annotated estimate beside the last trusted
fix, with the interval between them, rather than hiding the aircraft.

## Why the flags word is capability-gated

Reading the bit is gated on `FSS_FEATURE_POSITION_FLAGS` (0x10), negotiated
through the existing version handshake.

A peer that predates the capability leaves the flags word at 0. Read
unconditionally, that means "no fix" — so every position such a client ever
sends would be marked dead-reckoned, `position` would never populate in
fss-web, and the operator would get a permanent GPS warning on a perfectly
healthy aircraft. Alarm fatigue is a real harm and this would have been a
guaranteed one.

The gate bounds the blast radius the other way too. The library advertises
`FSS_SUPPORTED_FEATURES` on the application's behalf, so an app that relinks
without populating flags claims the capability it does not honour. That is
accepted: the cost is that its reports read as dead-reckoned, and the two known
client implementations both set the bit correctly — cap-fmu drives it from
`GPS_RAW_INT.fix_type`, and fss-adsb always sets `valid_coords` because it only
builds a report once a message carried a position.

## Session scope and logging

Storage is per report; logging is edge-triggered — loss, restore, and a
reminder every hundredth report so a long outage does not go silent between the
edges. An aircraft streams position at up to 5 Hz, so an unthrottled line per
report would bury every other message for the length of the outage.

The fix-state memory (`gps_fix_state_known` / `gps_fix_valid` on the session) is
session-scoped, so a reconnect mid-outage re-logs the loss. That is correct
rather than a duplicate: a new session genuinely has no prior state, and the
stored rows — which are what fss-web reads — are unaffected either way.

The restore edge is new. Previously the log said an aircraft had lost its fix
and then never mentioned it again.

## Deployment

fss-web migration 0016 must be applied before this server release starts;
`required_columns[]` now includes `gps_fix_valid`, so the startup schema check
refuses to run without it (see
[decision 73](73-startup-schema-verification.md)). The e2e mirror
`e2e/schema/001_init.sql` moved in the same commit.

cap-fmu needs no matching change (`cap-fmu` todo/107, closed 2026-08-01). It
sends the estimator's finite coordinates with the coords-valid bit clear, and
always has; a change to NaN them was written and reverted once the
peer-broadcast cost surfaced. So the flag-only row — stored, relayed, marked —
is the case a real GPS outage produces, and the NULL-geometry row is for a
client with no estimate to offer at all.

What cap-fmu does need is a rebuild against a client library that advertises
`FSS_FEATURE_POSITION_FLAGS`: the flags word is only read once the capability
is negotiated, so a deployment that updates the server but not the aircraft
keeps reporting a blind aircraft as GPS-backed. `CAP/test-plan`'s Tier-3 Path M
`m03` holds a strict xfail on the operator-visible end state and will XPASS
once both ends are deployed.
