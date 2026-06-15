# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- The position-staleness gate now corrects for a measured client↔server clock
  offset, so a client whose clock is skewed but whose link is healthy no longer
  has all its positions dropped. The offset is estimated from RTT responses: a
  peer that negotiates the new `rtt-offset` capability stamps its wall clock
  into each `rtt_response`, and the server folds `client - server` (bounded by
  half the round trip, smoothed across samples) into the gate. The stored
  timestamp is never rewritten — only the accept/reject window shifts. A peer
  that does not negotiate the capability is unaffected and the gate stays a
  plain symmetric window.
- Optional-capability negotiation in the protocol version handshake. The
  handshake's `feature_flags` field is now a bitmask of capabilities each peer
  supports; the negotiated set is the intersection of the two, recorded on the
  connection. This lets new optional features (see todo/17) be added without a
  protocol-version bump — a peer that does not advertise a capability never has
  it used, and a peer cannot enable one this build does not implement. No
  capability bits are enabled yet, so this is wire-compatible with existing
  peers (which advertise none).

### Security
- The TLS handshake no longer runs inline on the accept thread. A peer that
  completed the TCP connection but then stalled the handshake (sending no or
  partial ClientHello) previously blocked the accept thread inside a blocking
  handshake `recv` with no timeout, halting *all* new connections to the
  server — a trivial unauthenticated denial of service. Connection setup now
  runs on a bounded pool of worker threads, each handshake is bounded by
  `gnutls_handshake_set_timeout` (configurable via `tls_handshake_timeout_ms`,
  default 10 s), and concurrent in-progress handshakes are capped
  (`max_concurrent_handshakes`, default 64) so the worker path cannot itself
  be used to exhaust threads/memory. The client side also bounds its handshake
  so a stalled server cannot hang `connectTo()`.

### Fixed
- A position report from an aircraft with no GPS fix is no longer recorded and
  broadcast as a real position at (0,0) — Null Island. The "no-fix" condition
  (a NaN coordinate) now travels the wire as an `INT32_MIN` sentinel that
  decodes back to NaN, so the server discards it (with a distinct, throttled
  "no GPS fix" warning) instead of storing a bogus 0°N 0°E telemetry point.
  Framing is unchanged, so no protocol-version bump is required; interop
  degrades safely (a new sender's no-fix sentinel is rejected as out-of-range
  by an old receiver, and an old sender's no-fix still arrives as (0,0) until
  the fleet upgrades).
- The position-report staleness check is now symmetric and skew-aware. It
  previously compared the client-stamped time one-sidedly against the server
  clock, so a client whose clock lagged >30 s had *every* position silently
  discarded (a healthy-looking link with a frozen track) while a client whose
  clock ran *ahead* bypassed the check entirely. The window now rejects
  future-dated reports too, escalates from a single WARN to an ERROR naming the
  suspected skew once a client trips it repeatedly (instead of a per-message
  WARN drip), and is configurable via `position_staleness_ms` (default 30000,
  0 disables). The gate carries a per-client clock-offset correction that is
  currently 0 — a later change will measure it so a skewed-but-healthy client's
  positions are accepted rather than dropped.

## [1.0.3] - 2026-06-13

### Security
- The server now verifies that a client certificate chains to the configured
  CA (and is unexpired/unrevoked) during the TLS handshake. Previously
  `GNUTLS_CERT_REQUIRE` only forced the client to *present* a certificate
  without validating it, so a self-signed certificate carrying a known asset
  CN would pass the handshake and satisfy the CN-based identity check — an
  authentication bypass. Legitimate clients signed by the project CA are
  unaffected.
- Fixed an out-of-bounds read decoding a message whose length-prefixed string
  field is positioned so its 8-byte alignment padding runs past the declared
  buffer length; the subsequent field read could pass a wrapped size_t bounds
  check and read past the buffer.

### Fixed
- A black-holed client connection (radio dropout with unacked data in
  flight) could stall a blocking send for the kernel's ~15-minute
  retransmission timeout; `TCP_USER_TIMEOUT` now bounds this to 30 s,
  matching the application-level client timeout.
- `sendCommand`/`broadcastMsg`/`sendSMMSettings` no longer hold the global
  client lock across blocking sends, so one stuck client cannot stall
  command dispatch to other aircraft or block the recv threads.
- The 15 s config tick no longer performs synchronous DB reads from the
  main loop; the server list and SMM settings are cached by the command
  poller thread, restoring the documented main-loop DB contract.
- `~server_clients` no longer joins connection recv threads while holding
  the client-list lock, fixing an intermittent shutdown deadlock.
- A command row with a NULL position or altitude is now refused at
  dispatch instead of decoding as latitude/longitude (0,0) — Null
  Island — or altitude 0 (descend-to-ground).
- `transport_ssl`'s `usable` flag is now atomic, fixing a data race
  between the recv thread, sender threads, and the destructor.
- The client reconnect backoff bookkeeping is now owned by a single
  thread, with the recv thread requesting resets via an atomic flag.
- Missing or unreadable CA/key/cert files no longer throw a raw
  `gnutls::exception` out of the client connection API; setup failures
  are logged and surfaced as `nullptr`/`false` returns.
- A signal interrupting a blocking `recv()` (EINTR) no longer tears down
  a healthy plain-TCP connection; the receive path retries, matching the
  existing send-path and TLS behaviour.
- The per-frame "Got a null msg" warning is throttled (first frame and
  every 100th), so a peer streaming undecodable frames cannot flood the
  log at line rate.
- A peer re-sending the wire-protocol version handshake after negotiation
  could downgrade the protocol version mid-session, and an out-of-sequence
  duplicate re-anchored the sequence check to its id — silently dropping all
  subsequent telemetry while the session still looked alive. Duplicates are
  now dropped with a throttled warning: the sequence advances only for an
  exactly in-sequence duplicate, the version is never re-negotiated, and the
  check is never re-anchored.
- `db_active_fss_servers_get` no longer appends a NULL entry to the
  active-server list when an allocation fails mid-cursor; the NULL acted as a
  premature terminator that truncated the list and leaked every entry after
  it. The append is skipped instead, keeping the array NULL-terminated.
- A malformed or wrong-typed configuration value (e.g. a string where a
  number is expected) no longer aborts the server through an uncaught
  `jsoncpp` exception or throws out of the client library constructor. The
  server now reads every value inside one guarded block and reports the
  offending file; the client contains the exception and behaves as it does
  for a missing config file.
- Hardened message decoding: capability ids outside the 0..63 bitmap no
  longer trigger shift undefined behaviour (they are dropped / read as
  false), and truncated `position_report`/`asset_command` frames decode their
  coordinates to NaN — rejected downstream as the honest "unknown" — instead
  of (0,0), which is Null Island, a legal coordinate that passes validation.
  Only reachable via a crafted direct `decode()`; the recv path already
  enforces the declared frame length.
- Failure-path cleanups: `setenv` in `db_connect` runs only before
  threads exist, DB read errors during identification are logged
  distinctly from "unknown asset", truncated SMM credentials are
  refused instead of shipped, and both `connectTo` paths handle
  `socket()` failure.

## [1.0.2] - 2026-06-09

### Fixed
- `fss_message_cb::conn` is now guarded by a mutex, eliminating a data race
  between the reconnect path (writer) and send/query paths (readers).
- `disconnect()` no longer nulls `conn` before joining the recv thread,
  preventing a null-pointer dereference in `processMessage()` during teardown.

## [1.0.1] - 2026-06-08

### Fixed
- Debian packages now install the correct headers

## [1.0.0] - 2026-05-16

### Added

- **Wire-protocol version handshake** — client and server exchange version
  numbers on connect; mismatched versions are rejected cleanly.
- **Async database writes** — all telemetry writes (position, status, RTT,
  search progress) are enqueued to a background thread, eliminating DB stalls
  from the main accept/dispatch loop.
- **Command delivery latency reduced to ~100ms** — command polling moved to a
  dedicated background thread on a 100 ms tick instead of the previous 1 s
  main-loop cadence.
- **DB write failure counter** — cumulative write failures are logged whenever
  the count changes, surfacing silent data-loss scenarios.
- **Certificate Revocation List (CRL) support** — server accepts a `crl_file`
  config key; SIGHUP triggers a live CRL reload and disconnects any currently
  connected client whose certificate appears on the new CRL.
- **Per-client token-bucket rate limiting** — configurable `message_rate_capacity`
  and `message_rate_refill` protect the server from burst flooding by individual
  clients.
- **In-order delivery and staleness checks** — per-connection sequence numbers
  reject out-of-order or duplicated messages within a session, and a timestamp
  staleness check discards heavily delayed position reports. (TLS already
  provides cryptographic replay/reorder protection at the record layer; these
  are application-level integrity checks, not a security replay defence.)
- **SMM credentials stored in `secure_string`** — SMM username and password are
  held in memory-locked, zero-on-free storage.
- **TLS cipher priority hardened** — GnuTLS priority string tightened; minimum
  GnuTLS version raised from 3.3.0 to 3.6.0.
- **Identity validation against leaf cert CN** — only the end-entity certificate
  Common Name is accepted as the asset name; intermediate CA CNs are ignored.
- **Non-aircraft identity validated against DB** — clients presenting a CN that
  is not enrolled in `assets_asset` are rejected after the TLS handshake; no
  telemetry rows are written for unknown assets.
- **Identify and disconnect events logged** — asset name, remote address, and
  reason are emitted at INFO level on every connect and disconnect.
- **Fake-client logs received commands** — the test fake client records commands
  it receives, enabling command round-trip assertions in e2e tests.
- **TCP keepalives and connect timeout** — `db_connect` sets
  `PGCONNECT_TIMEOUT=10` and TCP keepalive parameters (`idle=10s`,
  `interval=5s`, `count=3`) so a dead Postgres connection is detected within
  ~25 s instead of the OS default of ~127 s.
- **Postgres port configurable** — `postgres.port` is now an explicit field in
  `server.json`; defaults to 5432 if absent.
- **Structured logging framework** — a new `fss-log.hpp` provides levelled,
  structured logging used across the server and client.
- **Bounded message queues** — outbound queues are capped with a drop-oldest
  policy so a slow or stalled peer cannot grow memory without limit.

### Changed

- **C++ standard raised from C++14 to C++17** — the build now requires a
  C++17-capable compiler.

- **DB reconnect on connection loss** — `db_connection` detects a dropped
  Postgres connection on each main-loop tick and reconnects automatically,
  replacing the previous behaviour of silently dropping all writes.
- **ECPG connection string** — switched to the `dbname@host:port USER … USING …`
  form for compatibility across libecpg versions (including PostgreSQL 16 on
  Ubuntu 24.04).
- **Fatal DB startup failure** — if the initial connection to Postgres fails,
  the server exits non-zero immediately instead of entering the accept loop
  with a broken DB handle.
- **SIGTERM handled for graceful shutdown** — `SIGTERM` now triggers the same
  clean teardown sequence as `SIGINT`, making the server systemd- and
  `docker stop`-compatible.
- **Required config fields validated at startup** — all mandatory JSON fields
  (`postgres.host/user/pass/db`, `ssl.ca_public_key`, etc.) are checked before
  any resource is allocated; a clear error is logged for each missing field.
- **`COMMAND_LEN` buffer widened to 16 bytes** — the ECPG receive buffer for
  asset command strings was 7 bytes, too small for valid command names.
- **Command construction validates type at construction** — `asset_command_unknown`
  is rejected before it can be dispatched; unknown command types are logged and
  discarded rather than sent.
- **DB main-loop invariant documented and enforced** — synchronous DB reads are
  banned from the main loop; all reads go through the background command poller.
- **Reconnect backoff hardened** — client reconnect backoff now adds jitter and
  is clamped to its cap instead of overshooting.

### Fixed

- Use-after-free hazard in the receive thread eliminated.
- `gnutls_x509_crt_init` and `gnutls_x509_crl_init` return values checked.
- `setsockopt`, `close`, and `shutdown` return values checked throughout
  transport layer.
- `strdup` return values checked in `server-db.pgc`.
- All `assert()` calls in hot paths replaced with logged error returns.
- Oversized incoming messages rejected before allocation.
- SMM credential memory zeroed with `explicit_bzero` before `free`.
- Snapshot taken before `sendRTTRequest` to avoid a deadlock on the client
  list mutex.
- All cppcheck findings eliminated; `.cppcheck-suppress` file removed.
- Various clang-tidy findings resolved (narrowing casts, C-style casts,
  `reinterpret_cast` via `void*` helpers, `emplace_back`, scoped locks, etc.).
- Liveness and timeout timing now use a monotonic clock, so wall-clock
  adjustments no longer disturb keepalive and reconnect timing.
- `send()` is retried on `EINTR` instead of dropping the connection, and
  `accept()` backs off on fd/memory exhaustion rather than spinning.
- `SO_REUSEADDR` set on the listening socket so the server can restart
  immediately without waiting for the socket to leave `TIME_WAIT`.
- Out-of-range inputs rejected — invalid position reports, GOTO commands with
  out-of-range coordinates, and out-of-range asset-command bytes are discarded,
  and coordinate/voltage packing guards against NaN and overflow.
- `tslc` field now serialized correctly in position reports.

### CI / Infrastructure

- e2e test job made a hard gate — CI fails if any end-to-end test fails.
- End-to-end negative tests added: unreachable DB host causes non-zero exit;
  unknown asset name produces no telemetry rows.
- End-to-end server-restart / client-reconnect test added.
- Thread-sanitiser (TSan) and wrong-CN rejection tests added.
- `docker/start-server.sh` builds `server.json` with `jq` to handle env vars
  containing shell-special characters safely.
- Added `.clang-format`; `check-code.sh` enforces formatting via
  `clang-format --dry-run -Werror` alongside cppcheck and clang-tidy, and the
  CI static-analysis job now runs `check-code.sh` directly.

### Packaging

- `debian/copyright` rewritten as a proper machine-readable DEP-5 file
  declaring GPL-2+; the `dh_make` template placeholders are gone.
- `debian/control`: every binary package now declares `${shlibs:Depends}` and
  `${misc:Depends}`, so `dpkg-shlibdeps` derives correct runtime library
  dependencies; `-dev` packages are no longer listed as runtime `Depends`.
- `gnutls-bin` added to `Build-Depends` — the test suite shells out to
  `certtool` to generate TLS fixtures.
- Added a `resolute` packaging variant (`debian/control.resolute`,
  `debian/flight-safety-system-server.install.resolute`).
- `make dist` now produces a complete, clean release tarball: it ships
  `.clang-format` and the `certs/` helper scripts, and no longer leaks
  developer-local Python virtualenvs or generated TLS fixtures. `make
  distcheck` builds the server, tests, and example client.
- `examples/` installs the generated `fss-server.service`, never the
  `fss-server.service.in` template.
- `.deb` packages verified to build for bookworm, trixie, noble, and resolute.

## [0.12.3] - 2025-11-07

[1.0.1]: https://github.com/canterbury-air-patrol/flight-safety-system/compare/1.0.0...1.0.1
[1.0.0]: https://github.com/canterbury-air-patrol/flight-safety-system/compare/0.12.3...1.0.0
[0.12.3]: https://github.com/canterbury-air-patrol/flight-safety-system/releases/tag/0.12.3
