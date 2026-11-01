# 87 — An explicit admission signal for non-aircraft sessions

## Problem

The client library reports a server as admitting it only after receiving a
command, server list or SMM settings (todo/79). The server sends none of those
to an identified non-aircraft client (ADS-B feeders, config tools): command and
settings delivery need an asset id, and periodic broadcasts are aircraft-only
(todo/59). A healthy feeder therefore reported `DISCONNECTED` indefinitely.

RTT cannot be the signal: the server sends it to every client in its list,
including a duplicate identity that is about to be refused (todo/31).

## Decision

A new message, `identity_accepted` (type 15, no payload), is sent by the server
to a non-aircraft client immediately after its identify is accepted. It is gated
by the new feature flag `FSS_FEATURE_IDENTITY_ACCEPTED` (0x20), so it is only
sent to a peer that advertised it. The client treats it as an admission signal
(`admits_client`).

Chosen over reusing the server-list message for feeders: that would have cut
across the todo/59 broadcast policy, overloaded the meaning of a server list,
and left a failed list read with no signal at all.

## Compatibility

- New feeder client, old server: the server never sends it, so the feeder keeps
  reporting `DISCONNECTED`. That is the existing behaviour and the conservative
  direction.
- Old feeder client, new server: the flag is not negotiated, so nothing is sent.
- Aircraft sessions are unchanged: they are admitted by the existing signals and
  are never sent `identity_accepted`.
- Rejected, pending and TLS-only connections are sent nothing, so they never
  count as service. Admission is cleared on close like any other (todo/79).

## Coverage

Unit: message round-trip; server sends only to a negotiating peer and never on
a refused identify; client admits on it, clears on close, earns it again after
reconnect. e2e (`test_adsb_forwarding.py`): a real feeder reports
`CONNECTED_1`; a feeder refused for using an aircraft's CN never does.
