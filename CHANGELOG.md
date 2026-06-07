# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.1] - 2026-06-08

### Fixed
- debian packages installing the correct headers

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

### Changed

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

[1.0.0]: https://github.com/canterbury-air-patrol/flight-safety-system/compare/0.12.3...1.0.0
[0.12.3]: https://github.com/canterbury-air-patrol/flight-safety-system/releases/tag/0.12.3
