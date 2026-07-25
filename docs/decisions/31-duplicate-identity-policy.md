# 31 — Duplicate asset identity: reject the newcomer by default

**Decided** 2026-07-05, race closed 2026-07-13 (todo/44). Shipped in 1.1.0 and
1.2.0. Config: `duplicate_identity_policy` in `server.json`.

## Context

Two clients identifying as the same asset had no defined behaviour: no dedup
existed at all, and both sessions went live. Applies to the aircraft path only —
non-aircraft clients never claim an asset id.

## Decision

Default **`duplicate_identity_reject_newcomer`**: keep the existing session and
refuse the newcomer.

Chosen to avoid flip-flopping on a genuine misconfiguration — the common real
cause of a duplicate is two processes pointed at the same certificate, and
eviction turns that into an endless sever/reconnect cycle across both. This is
the default *even though* mTLS makes a stolen-key hijack the less likely reading
of "duplicate": the failure mode of the wrong choice is worse for
misconfiguration than for hijack, and a hijack is already gated by possession of
a CA-signed key.

`evict_oldest` is available for deployments that would rather favour a fast
reconnect after an unclean client death.

**This decision is mirrored in the CAP master plan** — change it in both places
or not at all.

## Why the check and the claim are one critical section

The first implementation checked *published* `cached_asset_id`s and left
publication to the caller, so two concurrent identifies for the same asset could
each miss the other's unpublished claim and both go live (todo/44).

`server_clients` now owns an `asset_owners` claim map guarded by its existing
`lock`, and the duplicate check plus the claim are a single critical section in
`resolveDuplicateIdentity()`. First claimant wins; `reject_newcomer` refuses the
loser, `evict_oldest` dethrones the recorded owner — exactly one, since claims
are unique. Publication timing no longer matters, because the check reads the
claim map.

Claims are released in `clientDisconnected()` by scanning for the client as
**owner**, not by its published id. Releasing by published id would leave an
asset permanently unclaimable when an identify was evicted or timed out before
it published.

The evictee's blocking `disconnect()` stays **outside** `lock`, preserving the
established lock-ordering rule.

## Where this is pinned

Both policy branches plus a distinct-asset-ids sanity case; the two distilled
interleavings with publication withheld (these fail deterministically on the
pre-fix code); concurrent identifies through the real `processMessage` path
(exactly-one-winner over 20 rounds, plus an `evict_oldest`
exactly-one-survivor and claim-transfer check); release-on-disconnect for both a
published and a never-published claim; and `e2e/test_duplicate_identity.py`,
which drives two real fake-client processes under the same certificate against
the real server.
