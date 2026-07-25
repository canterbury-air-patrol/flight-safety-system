# 23 — Supported scale: tens of aircraft, not hundreds

**Standing assumption.** Recorded 2026-07-24; the batched command poll that
raised the read ceiling shipped the same day.

## The assumption

The server is sized for RPAS fleets of **tens of aircraft** against a single
local PostgreSQL+PostGIS database. Every capacity judgement in the codebase — the
single read connection, the per-connection thread counts, the poll cadence — is
made against that number.

It is stated in the README's *Supported scale* section for operators; this file
is the engineering record of *why* it is where it is, and what to do about it.

## What actually sets the ceiling

Two things scale with the number of connected aircraft, and one no longer does.

- **No longer:** the command poller used to issue one `getCommand(asset_id)`
  read per client per 100 ms tick, all serialised on the single read connection
  — about 10·N reads/second. It now issues one batched
  `DISTINCT ON (asset_id)` query per tick, so the steady-state command-read rate
  is a constant ~10 queries/second regardless of N.
- **Threads:** roughly 2 per client, plus the handshake worker pool.
- **The single read connection** remains a serialisation point for everything
  the poller does.

## If a deployment approaches ~100 concurrent aircraft

In order:

1. Re-measure. The batched poll already removed the original bottleneck, so the
   next limit is unknown rather than predicted.
2. Reconsider the per-connection thread model — it is the remaining term that
   grows linearly.
3. Then consider LISTEN/NOTIFY push in place of polling, or a read-connection
   pool. Note that FSS never inserts commands itself (the external fss-web does),
   so a NOTIFY-based push requires a trigger on the fss-web side plus libpq
   here — it is a cross-repo change, not a local one.

## Related

The broadcast relay's O(N²) decode cost was a genuine super-linear term and was
fixed independently (todo/55): `broadcastMsg` now packs a position report once
and `sendPacked` stamps each recipient's sequence id into a copy of those bytes,
instead of decoding once per recipient and re-packing each clone.
