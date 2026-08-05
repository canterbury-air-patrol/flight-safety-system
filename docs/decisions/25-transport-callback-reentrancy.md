# 25 — Transport callbacks run with the connection's message lock released

**Standing constraint on the transport API.** Decided and shipped 2026-07-25,
released in 1.3.0. Carries an ABI break: sonames `.so.3` → `.so.4`.

## The constraint

`fss_connection` delivers `fss_message_cb::processMessage()` with `msg_lock`
**released**. A handler may therefore re-enter the connection's message-queue
API from inside its own callback — `getMsg()`, `setHandler()`, `disconnect()`,
or anything reaching them — without deadlocking.

Anyone writing an `fss_message_cb`, in this repo or a consumer, may rely on
that. Anyone changing the transport must preserve it.

## Why it is written down

The original implementation held `msg_lock` across the callback, so any such
re-entry self-deadlocked on a plain `std::mutex`. It was never a *live* bug —
the in-tree handlers had been hand-shaped around it — which is precisely why it
survived so long. The API was the hazard, for every future callback, and the
hazard was invisible from the call site.

## What upholds it

Delivery bookkeeping guarded by `msg_lock` (`delivery_cv`, `delivery_depth`,
`delivering_thread`) restores the two properties lock-held delivery gave for
free:

- deliveries on one connection stay **serialized** — a would-be deliverer waits
  for `delivery_depth == 0`;
- `setHandler()` and `detachHandler()` **wait for an in-flight callback to
  return** before swapping or clearing the handler. That wait is the lifetime
  proof for the non-owning `fss_message_cb *`: it cannot be destroyed while a
  call into it is in flight.

The delivering thread is exempt from the wait, so a re-entrant call made by the
callback itself proceeds instead of blocking on itself.

`disconnect()` gained the same wait, which also closed a pre-existing hole in
its two thread-detach branches — the server-side client teardown path's only
barrier.

`detachHandler()` exists as its own entry point rather than as
`setHandler(nullptr)` because installing a handler flushes the backlog into it
and can therefore propagate an exception the handler threw, while detaching runs
no callback and cannot. That distinction is what lets `~fss_message_cb` detach
without a try/catch, and stating it in a signature keeps a later change to the
flush semantics from silently making the destructor a throwing path.

## What this does *not* fix

**Cross-thread lock-order cycles.** `setHandler()` still blocks on a running
callback, so the "call `activate()` / `reconnect()` outside our own lock" rules
in `server-clients.cpp` and `client-ssl.cpp` remain load-bearing. Removing them
because callbacks are "lock-free now" would reintroduce a deadlock.

## Where this is pinned

Five regression tests in `tests/handler_reentry_test.cpp`, each verified to hang
against the pre-fix transport, plus a TSan `setHandler`-churn case.
