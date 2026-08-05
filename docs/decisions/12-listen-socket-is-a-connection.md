# 12 (C7) — `fss_listen` inherits from `fss_connection`, deliberately, for now

**Decided** when todo/12 group C was assessed; promoted here 2026-08-05 by the
todo/75 orphan audit, which found this rationale existing **only** as a comment
in `src/fss-transport.hpp` — one refactor away from being lost, in a file whose
shape it explains.

## The wart

`fss_listen` is-a `fss_connection`. A listening socket is not a connection: it
never sends a message, never receives one, and never negotiates a version. What
it actually wants from the base class is the file-descriptor and receive-thread
lifecycle — `shutdownSocket()`/`disconnect()`, the fd ownership rules, the
thread join discipline. What it *inherits* is all of that plus `sendMsg()`,
`sendPacked()`, `getMsg()`, the message queue, the sequence counter and the
negotiated-version state, every one of which is meaningless on a listener.

The clean shape is a small `fd_owner` base holding the fd plus the thread
lifecycle, with `fss_listen` and `fss_connection` as siblings on top of it.

## Why it has not been done

It is an ABI break, and a large one: `fss-transport.hpp` is an installed header
exported by all four libraries, and the change moves members between classes and
rewrites a vtable. Under this project's soname rule
([the release checklist](../release-checklist.md), step 1) that costs a
`-version-info` bump and a rebuild of every consumer.

The bump itself is not the objection — this project breaks its ABI when it has a
reason to, and has done so twice in the 1.x series. The objection is doing it
*for this alone*: consumers pay a rebuild and the risk of a hierarchy rewrite in
the transport layer, and get in exchange a cleaner class diagram and no
behavioural improvement whatsoever. The right time is a release that is
restructuring the transport for a reason that pays for the risk, and that has not
happened yet.

**Note for whoever picks this up:** "the next release that breaks the ABI" is
*not* the trigger, and reading it that way is the likely mistake. A release
whose soname bump comes from an unrelated change has not made this refactor any
safer to do — it has only made it cheaper to ship. The trigger is a transport
rework substantial enough that the hierarchy is being touched anyway.

## What holds it together in the meantime

Nothing in the base class is *wrong* for a listener, only unused: the inherited
send/receive members are never called on an `fss_listen`, because nothing hands
one to code that sends. The cost is comprehension, not correctness — a reader of
`fss_listen` has to work out which half of its inherited surface is real.

## Related

- `src/fss-transport.hpp` carries the short form at `class fss_listen`.
- [The release checklist](../release-checklist.md) — step 1 is where the ABI
  consequence above is enforced.
- The C8 invariant from the same todo/12 group (a single `fss_message` must not
  be sent concurrently on multiple connections) is documented at `sendMsg()` and
  `sendPacked()` in the same header; it is an invariant a caller must respect,
  so it stays inline.
