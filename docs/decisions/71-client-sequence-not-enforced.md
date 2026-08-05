# 71 — The v2 sequence check is server-side only

**Decided** 2026-08-05, documentation only: no code changed, and none is
intended to. The asymmetry described here has existed since protocol v2
shipped; what was missing was any statement that it is deliberate.

## Context

Protocol v2 numbers every message a peer sends with a monotonic per-connection
counter (`fss_connection::getMessageId()`). The **server** enforces it:
`client_session.cpp` anchors `expected_seq` at the version handshake, requires
each subsequent `fss_message::getSeq()` to match, and disconnects the session on
a mismatch — todo/39 chose disconnect over a silent drop, because `expected_seq`
only advances on a match, so dropping would freeze the session until the 30 s
liveness timeout.

`client_ssl::fss_server::processMessage()` does no such check. It negotiates the
version, stores it on the connection, and then dispatches on message type. A
server that reordered, duplicated or skipped a message id would be accepted
silently by every client.

The server's own comment already states the guarantee this rests on:

> This is an application-level integrity check that rejects reordered or
> duplicated messages within a session — it is **NOT** a security replay
> defence. Under its security assumptions TLS already provides replay and
> reorder protection at the record layer.

## Decision

The asymmetry stands, and is recorded rather than removed.

- **TLS does the real work.** If the record layer already rules out genuine
  reorder and replay, a second implementation on the client adds nothing against
  an attacker. The server-side check earns its place for a different reason —
  see below — not as a defence.
- **The two sides face different risks.** The server accepts connections from
  many peers of unknown quality and must be able to reject one cheaply. The
  client is the resource-constrained side, running on the aircraft, and talks
  only to servers the operator configured or a configured server vouched for.
- **The client's residual value is diagnostic, not defensive.** The server-side
  check also catches *local framing bugs* — todo/39 names a consumed-but-never-
  sent id (todo/38) as a cause a mismatch would surface. The server→client
  direction has the same failure modes (`sendPacked`'s id rollback, `sendMsg`'s
  rollback) and nothing would notice. That is a real, if small, loss, and it is
  the reason option 2 below stays open rather than being rejected outright.

## Rejected: mirroring the server, disconnect included

This is the change a reader who spots the inconsistency will reach for, so it is
recorded as rejected explicitly.

An aircraft that drops its server connection over one bad sequence number has
traded a diagnostic for a safety regression: that connection carries TERM,
RTL and DISARM. The server dropping one aircraft costs that aircraft a
reconnect; the client dropping its server costs the operator their control link,
and does so on exactly the kind of transient the check is least able to
attribute. Whatever a client does about a mismatch, it must not be that.

## Left open: a log-only check

Tracking `expected_seq` client-side, logging a **throttled** WARN on mismatch,
advancing to the observed value and never disconnecting would recover the
diagnostic without the availability risk. It is small, needs no wire change and
no schema change, and the state fits on `fss_connection` beside
`negotiated_version`.

It is not taken here because it is not free: `fss_connection` is in the
installed ABI, so a new data member costs a soname bump
(`docs/release-checklist.md`, step 1) and must therefore ride a release that is
already breaking the ABI for another reason. Worth more now than when the item
was filed, because `sendPacked()` stamps ids directly into cloned bytes and
bypasses `fss_message::setId` — a newer path than the sequence check itself. If
it is ever taken, the acceptance test is a client connection driven with a
deliberately skipped id asserting exactly one throttled WARN and **no**
disconnect.

## Related

- `src/fss-client-ssl.hpp` carries the short form of this at
  `fss_server::processMessage()`, which is where a reader meets the asymmetry.
- [25 — transport callbacks run without the connection's lock](25-transport-callback-reentrancy.md)
  covers the other standing property of the same callback path.
