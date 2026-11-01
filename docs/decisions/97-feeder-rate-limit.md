# 97 — Separate rate limit for non-aircraft clients

## Problem

Every connection shared one token bucket (`message_rate_capacity` 100,
`message_rate_refill` 20/s) across position, status and search messages. That is
ample for an aircraft at ~5 Hz, but a non-aircraft ADS-B feeder relays many ICAO
addresses over one connection, so it exceeded 20 msg/s and silently lost
positions, visible only as rate-limit log lines.

## Decision

- A session that identifies as non-aircraft swaps its bucket for
  `feeder_message_rate_capacity` / `feeder_message_rate_refill`, defaulting to
  **1000 burst / 200 per s** (10x the aircraft default). The role is only known
  at identify time, so the swap happens there, on the recv thread that owns the
  bucket. Aircraft keep the original limits and the `command_ack` bucket is
  unchanged.
- Zero values are rejected with a warning and the default used, for the same
  reason as the aircraft limits (a zero bucket drops everything forever).
- Messages lost to the limit are counted per session
  (`fss_client::getRateLimitDropped()`, cumulative) in addition to the existing
  throttled log line.

## Sizing

A feeder relaying N aircraft at the 1 Hz ADS-B update rate needs about N msg/s;
200/s covers roughly 200 simultaneous aircraft with a 1000-message burst for
reconnect backlogs. Raise `feeder_message_rate_refill` for denser receivers.
Per-type buckets were not needed: the problem was the per-connection total, not
one type starving another.
