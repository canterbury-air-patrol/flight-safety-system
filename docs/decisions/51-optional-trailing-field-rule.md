# 51 — Optional trailing wire fields are prefix-closed

**Decided** 2026-07-18. Shipped in 1.2.0 as documentation only; the rule is
recorded beside the `FSS_FEATURE_*` block in `src/fss-transport.hpp`, with
pointer comments at both fields it governs.

## Context

Two messages carry an optional trailing field:
`fss_message_rtt_response::client_timestamp` (todo/17 item 3) and
`fss_message_asset_command::server_command_id`
([49](49-server-command-id-semantics.md)). Both are omitted when 0 so that a
peer which does not implement the feature sees a frame it already understands.

`fss_message::decode()` is **static and connection-blind**: it has no access to
the negotiated `feature_flags`. An optional field's presence can therefore only
be inferred from the remaining frame length.

## Decision

Presence stays length-inferred, and the optional tail is **prefix-closed**: the
day a message gains a *second* optional trailing field, that new field may be
emitted only together with every earlier optional field of that message —
0-filled if unset or unknown — never on its own.

A peer implementing the new field always emits both, so a frame carrying only
one optional field can only mean a peer unaware of the new field; the 0-filled
earlier field decodes to its existing "not reported" sentinel, which every
consumer already handles. Emitting the new field alone would be
indistinguishable *by length* from today's one-field dialect.

## Alternative not taken

Making presence explicit — a presence bitmap or TLV encoding for the optional
tail — is the structurally correct answer, and is **not possible without a
protocol v3**: it changes the framing every existing peer parses. If a v3 ever
happens, fold these optional tails into an explicit presence mechanism and
retire this rule.

Passing the negotiated `feature_flags` into `decode()` was also rejected: it
would make decoding connection-dependent, which is precisely the property that
keeps `decode()` testable in isolation and usable by the fuzz corpus.
