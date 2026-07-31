# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **The server verifies the database schema at startup and refuses to run
  without it** (todo/73,
  `docs/decisions/73-startup-schema-verification.md`). The tables FSS reads and
  writes belong to `fss-web`, a separate repository, and are created by its
  Django migrations; nothing checked that the deployed database actually had
  them. A deployment whose `fss-web` had not applied the migration carrying the
  command-ack columns started cleanly, accepted every aircraft, and then severed
  the entire fleet on the first command dispatch — permanently, because the
  fail-safe cannot clear while the schema is still wrong. The server now
  anti-joins every column its statements touch against `information_schema`
  (plus the PostGIS extension) before the listener binds, and refuses to start
  naming what is missing. A column merely declared wider than the host buffer it
  is read into warns instead of refusing, so a benign widening migration cannot
  itself cause an outage. There is no config key to skip the check.
  **Deployments require `fss-web`'s `assets` migration 0008 or later**
  (`dispatch_id`, `ack_state`, `ack_timestamp`, `ack_superseded_by` on
  `assets_assetcommand`); this is now stated in the README and
  `docs/release-checklist.md`. Provisioning the e2e schema from `fss-web`'s real
  migrations, which would catch a breaking migration when it lands rather than
  when it deploys, remains open.
- A tracked design record under `docs/` (todo/54). The project's settled design
  decisions previously lived only in the gitignored `todo/` directory, which
  code comments, changelog entries and commit messages all cite by number — so
  every one of those citations was a dangling pointer in any other clone, and
  the only copy of the decision history was one developer's working tree.
  `docs/decisions/` now holds one file per settled decision, keeping the `todo`
  numbering so existing citations stay resolvable (`todo/49` is
  `docs/decisions/49-server-command-id-semantics.md`), and each records the
  alternatives that were rejected along with the evidence — the part that cannot
  be recovered from the code. `docs/release-checklist.md` is new, and carries
  the ABI check (`abidiff` the installed headers and libraries against the last
  tag, *before* choosing the version number) that todo/61 left outstanding after
  an ABI break reached a tag unbumped. Open work items stay in `todo/`.
  Citations in the public headers and the fail-safe monitor now point at the
  tracked documents; the rest are retargeted as those files are next touched.

### Changed
- **The DB fail-safe's thresholds are measured in real time, not in loop
  iterations** (todo/70). `db_failsafe` opened each tick with `elapsed_secs++`
  and compared every threshold against that counter, but the caller advances it
  on a 100 ms sleep *plus* whatever the loop body took — a fan-out over every
  client, and once a second a pass that joins departing clients' receive
  threads — and the signal handlers install without `SA_RESTART`, so a SIGHUP or
  SIGTERM returned the sleep early. "5 seconds of sustained failure before
  severing the fleet" therefore meant five loop iterations: stretched under
  load, compressed under signal traffic. Load correlates with the database being
  unwell, so the trip ran late in exactly the conditions it exists for. This was
  also the only timekeeping class in the tree without an injectable clock, which
  is why the drift had never been measured — its tests could only assert the
  counter arithmetic. It now takes an `IClock`, and the main loop runs every
  periodic task on its own deadline, so `checkTimeouts()` and the RTT sweep keep
  honest time as well. No threshold or transition changed; the existing twelve
  cases are unchanged and six new ones pin the edges (no trip at 4.9 s, trip at
  5.1 s; recovery at 15.001 s but not at 15.000 s; one six-second tick trips
  where five calls would not have; thirty signal-shortened ticks inside a second
  do not). The trip log now names how long the incident had been running.
  **The config field `db_write_failure_disconnect_ticks` is renamed
  `db_write_failure_disconnect_secs`** — which is what it always meant, and what
  its sibling was already called; the old key is still accepted and warns.
- **A command issued once no longer destroys and rewrites its own stored
  acknowledgement every 10 seconds** (todo/68,
  `docs/decisions/68-command-redelivery-and-ack-keying.md`). Nothing retires a
  command row, so the server re-dispatched an asset's newest command every 10 s
  for as long as it stayed connected — and every redelivery recorded a new
  dispatch, which clears `ack_state`, `ack_timestamp` and `ack_superseded_by` to
  reopen the ack cycle. The stored outcome of a command was therefore erased and
  re-established on a 10-second loop, indefinitely, on the healthy path: an
  operator or investigator saw the ack for the most recent redelivery, never the
  aircraft's original response, and an aircraft that went quiet mid-cycle left
  the columns reading NULL. It also put a permanent floor of ~18 non-evictable
  writes per minute per aircraft under the queue whose first dropped command
  write severs the fleet. Three changes: acks are matched on the command row's
  primary key, translated in-session from the per-connection id the aircraft
  echoes (no wire or schema change), which also means an ack for a dispatch the
  session never made is dropped rather than reconstructed onto a plausible row;
  a dispatch is recorded once per connection instead of once per resend; and
  redelivery stops once the aircraft reports a terminal outcome. The redelivery
  stop is scoped to the *connection*, never to the command row — an aircraft has
  no persistent storage, so a restart, link drop or server bounce arrives as a
  new session that dispatches again immediately and keeps resending until that
  connection acks for itself. An autopilot rebooting underneath a live client
  connection is the one case the server cannot see; it is raised with cap-fmu.
  Decision 48's fencing argument is revised accordingly, and decision 49's claim
  that `dispatch_id` is "stable across resends of that delivery" is corrected —
  it never was, and the same wording was repeated on the `server_command_id`
  field comment in `fss-transport.hpp`.
- **ABI break:** all four library sonames bump `.so.3` → `.so.4`
  (`-version-info 4:0:0` for `libfss`, `libfss-transport`,
  `libfss-transport-ssl`, `libfss-client-ssl`). Two independent layout changes
  land in this release and one bump covers both, since `.so.4` has not been
  released: the installed `fss-transport.hpp` changed object layout since
  1.2.1, where `fss_connection` gained the todo/25 delivery members and the
  todo/52 deferred-close descriptor; and the installed `fss-client-ssl.hpp`
  added data members and a virtual to `client_ssl::fss_server` and
  `client_ssl::fss_client`. A same-soname mix of an old library with the new
  header (or vice versa) would silently corrupt derived-class layouts rather
  than fail to link, so consumers must rebuild. See
  `docs/release-checklist.md`.
- `client_ssl::fss_client::sendMsgAll()` and `attemptReconnect()` are now
  **non-blocking**: each `fss_server` owns an outbound worker thread that
  performs its own blocking I/O
  (`docs/decisions/66-67-client-outbound-fanout.md`, todo/66). Both were serial
  loops of blocking calls on the caller's thread, so a server that completed TLS
  and then stopped reading held telemetry to every healthy server behind it for
  a whole `TCP_USER_TIMEOUT` (30 s by default), and a reconnection pass cost the
  sum of every unreachable server's connect and handshake timeouts. The shipped
  example drives both from one thread and the README names it as the starting
  point for a real client, so the aircraft's telemetry cadence and reconnect
  latency were set by the slowest server rather than the fastest. This is the
  aircraft-side counterpart of the hardening todo/21 + todo/36 did on the
  server. `sendMsgAll()` packs the message once and shares the frame read-only
  across the workers, each stamping its own sequence id via
  `fss_connection::sendPacked()` — which is what keeps the todo/12 C8 invariant
  intact now that the sends are concurrent. Per-server queues are bounded and
  drop the OLDEST frame under backlog (telemetry is loss-tolerant); the loss is
  reported by the new `fss_server::getDroppedSends()`.
- A `client_ssl::fss_client` now expires servers it learned from a server-list
  broadcast (todo/67). `updateServers()` only ever added — there was no removal
  path in the client at all — so a server deactivated in `config_serverconfig`
  was carried by every client that had ever seen it for the process lifetime,
  costing a blocking connect attempt per backoff interval, and silently: nothing
  logged it. A learned server absent from every list for
  `learned_server_expiry_ms` (new client config field, default 60 s = four 15 s
  broadcast rounds; 0 disables) is now dropped with a WARN naming it, and no
  more than `max_learned_servers` (new, default 16) are ever learned. Servers
  from the config file are exempt from both — they are the operator's declared
  intent and must survive an outage that empties every broadcast list. New
  `getServerCount()` / `getLearnedServerCount()` make what a client is carrying
  visible.
- `fss_connection` now invokes `fss_message_cb::processMessage()` with its
  internal `msg_lock` released, instead of holding it across the callback
  (todo/25). Holding a transport mutex across arbitrary user code meant any
  handler that re-entered the connection's message-queue API from inside its own
  callback — `getMsg()`, `setHandler()`, `disconnect()`, or anything reaching
  them — self-deadlocked on a plain `std::mutex`. The in-tree handlers were
  hand-shaped to avoid it, but the API itself was the hazard for every future
  callback. New delivery bookkeeping preserves the two properties the lock-held
  shape provided: deliveries on one connection stay serialized (a would-be
  deliverer waits for delivery-idle first), and `setHandler()` — including the
  `setHandler(nullptr)` in `~fss_message_cb` — waits for any in-flight callback
  to return before swapping the handler, which is the lifetime proof for the
  connection's non-owning `fss_message_cb *`. The delivering thread is exempt
  from that wait, which is what makes re-entrant calls work. `disconnect()`
  gained the same wait, so its two thread-*detach* paths now also guarantee no
  callback is in flight when it returns — previously they guaranteed nothing,
  which was the only barrier the server-side client teardown path had.
  Cross-thread lock-order cycles are unaffected: `setHandler()` still blocks on
  a running callback, so the existing "call `activate()`/`reconnect()` outside
  our own lock" rules in `server-clients.cpp` and `client-ssl.cpp` remain
  load-bearing. No behaviour change for existing handlers.
- Broadcast relays now pack the source message once and share that frame,
  read-only, across every recipient instead of decoding a fresh clone per
  recipient (todo/55). Previously each relayed position report to N connected
  aircraft cost N+1 full message decodes (heap allocation, a field-by-field
  buffer walk, a `std::string` callsign) plus N re-packs; fleet-wide, that relay
  work grew as O(N²) decodes/second. A new `fss_connection::sendPacked()` copies
  the shared packed bytes and stamps the per-connection sequence id straight into
  the header at send time, so each recipient still gets its own uniquely-stamped
  frame (the C8 no-shared-instance invariant is preserved) but the per-recipient
  decode and re-pack are gone. Delivered behaviour per recipient is unchanged;
  this raises the fleet size the server sustains before broadcast fan-out becomes
  the bottleneck. Companion to the todo/23 batched command poll below.
- The command poller now issues a single batched database read per tick for
  the newest pending command across all connected aircraft, instead of one
  `getCommand` query per connected aircraft (todo/23). This drops the
  steady-state command-read load on the single serialised read connection from
  ~10*N queries/second (N = connected aircraft) to a constant ~10/second,
  raising the fleet size the server sustains before that connection becomes the
  bottleneck. Behaviour per aircraft is unchanged: each still receives its own
  newest command, an asset with no pending command is cleared exactly as
  before, and the resend-window dedup still governs what reaches the aircraft.
  A new `DISTINCT ON (asset_id)` cursor read (`db_asset_commands_get`) backs the
  new `IDatabase::getCommands` batch method; the single-row `getCommand` is
  retained for the identify-time dispatch path.

### Security
- `secure_string::operator==` (both the `secure_string` and `string_view`
  overloads) now compares in constant time — an accumulated XOR over the
  full length rather than a short-circuiting `memcmp` — instead of a
  documented "don't do this" boundary (todo/57). No production caller
  verifies a secret against untrusted input today (credentials are carried
  to aircraft, never checked locally; the operators are exercised only by
  tests), so this closes a latent timing side channel before a future
  caller could reach for the type's `==` in good faith and introduce one.
  Length is still checked first (short-circuiting) since length is not the
  secret; only content comparison is walked to completion regardless of
  where a mismatch falls.

### Fixed
- **A database read failure is no longer reported as an empty result on the
  command and SMM paths** (todo/69, extending
  `docs/decisions/24-database-read-error-control-flow.md`). Two of the four
  reads never adopted the convention that a read which could only produce a
  partial or misleading result reports that by status. `getCommand` returned
  `nullptr` for a failed `SELECT`, which the interface documents as "no pending
  command" — on the read that carries `TERM` and `DISARM`, so a read outage
  presented as "this aircraft has nothing waiting". `getSmmSettings` did the
  same, and `refreshSmmSettings()` wrote that result straight over its cache, so
  one transient failure on the poller thread wiped an aircraft's SMM settings
  until a later read succeeded. `getCommands` (the batched poll) did report the
  error and then discarded it, and `pollCommands()` cleared `pending_command`
  for every asset absent from the partial map — including every asset the read
  never reached. All three now return `std::optional`, where `nullopt` is the
  read having failed and an engaged empty value is the genuinely-absent answer;
  the callers keep their cached settings, leave pending commands untouched, and
  retry on the next tick. A row whose command string or credentials were
  truncated stays a per-row absence rather than a read failure: it is named on
  stderr and is unusable either way, and failing the batched read there would
  blind fleet-wide command polling on one bad row. No read now reports failure
  in-band.
- Connection teardown no longer closes the socket before the threads that
  might still use it are joined (todo/52). `disconnect()` used to shutdown
  *and* close the descriptor up front, then join the recv thread — so a
  recv/send that had already loaded the old fd number could, after an
  adversarially timed fd reuse (e.g. a concurrent accept in the TLS
  setup-worker pool), perform I/O on an unrelated connection: cross-session
  data injection. Teardown is now shutdown-first (which is what actually
  unblocks a stalled recv()/send(); the I/O loops bail on the retired fd),
  with the close deferred until the recv thread is joined; a recv-thread
  self-disconnect (garbage-frame threshold, oversized frame) defers the
  close to the owner's later disconnect() or the destructor, since it
  cannot join itself and a server-side outbound worker may still be
  mid-send. The server's `fss_client::disconnect()` uses the new
  shutdown-only primitive (`fss_connection::shutdownSocket()`) to quiesce
  its outbound worker between shutdown and close, closing the same window
  for its sender thread; and the TLS destructor no longer sends the gnutls
  close-notify once the descriptor has been retired — gnutls holds the raw
  fd number, so that write could likewise land on a reused descriptor. Not
  observable by TSan or the e2e suite (fd-number reuse is not a data race);
  unit tests pin the new ordering with fcntl probes instead.
  `fss_connection` gained a data member — an ABI layout change to the
  installed `fss-transport.hpp`, covered by the `.so.3` → `.so.4` soname
  bump recorded below.

### Added
- The server now logs every silently-rejected aircraft and non-aircraft
  identify: an ERROR naming both sides of a certificate-CN/identity
  mismatch, a WARN naming an identity with no matching asset in the
  database, and a separate ERROR when the asset lookup itself failed (e.g. a
  DB read outage) rather than folding that case into the "no matching asset"
  wording (todo/60). `IDatabase::getAssetId()` now returns
  `std::optional<uint64_t>` — nullopt for a failed read, 0 for a
  definitively unknown asset, matching the `getActiveServers()` convention —
  so a read outage during identify shows up in the logs as a DB incident
  instead of looking identical to a fleet of misconfigured certificates.
  Previously all these paths severed the connection without a single log
  line, so a permanently rejected client — e.g. an unregistered asset name,
  or every client during a DB read outage — was indistinguishable from
  network flapping at every layer's logs (this cost a day of misdiagnosis in
  the todo/65 re-test).
- `docker/start-server.sh` now accepts an optional `CRL_FILE` env var and,
  when set, adds it as `ssl.crl_file` in the generated `server.json`
  (todo/62). Previously the entrypoint's generated config had no way to
  reach `crl_file` at all — the server and `transport-ssl.cpp` have
  supported it (and its SIGHUP reload) since todo/29, but a container built
  from this image could only get CRL support by bind-mounting a
  hand-written `server.json` over the entrypoint's DB/port wiring entirely.
  Omitting `CRL_FILE` reproduces the previous config byte-for-byte.

### Changed
- `server_clients` (the client-list/asset-claim/degraded-gate class used by
  `server.cpp`) now splits its declaration and implementation across
  `src/server-clients.hpp` and a new `src/server-clients.cpp`, matching the
  header/impl split already used elsewhere (`fss-server.hpp` +
  `client_session.cpp`, `db-write-queue.hpp` + `.cpp`) instead of being the
  one ~450-line exception with every method body inline in the header
  (todo/56). Trivial one-liner accessors stay inline; the header now only
  declares the nontrivial methods. No API, ABI, or behaviour change — the
  class is not part of any installed header.

## [1.2.1] - 2026-07-19

### Fixed
- The listener's connection-setup worker now retires an accepted connection
  that its connect callback declines (returns false) or abandons by
  throwing. Previously the worker discarded the callback's verdict and
  dropped its reference, while the connection's own recv thread kept the
  connection alive — an unreachable zombie session leaking a socket and a
  thread per declined peer. Latent rather than live: the bundled server's
  callback never declines, but the callback's documented bool return
  invited exactly this usage.
- Client reconnect no longer loops forever after a liveness timeout
  (todo/65, found by CAP test-plan Path G's mixed-version interop tests).
  `fss_server::reconnect()` now retires the superseded connection properly —
  handler detached, socket disconnected, recv thread joined — instead of
  just dropping its reference, which leaked a thread + fd per reconnect,
  left the old session live at the server (where the default
  `reject_newcomer` policy then refused the client's own re-identify as a
  duplicate), and let the old connection's eventual closed event tear down
  the healthy replacement. Liveness also restarts in the cold-connect
  disarmed state on a fresh connection, so a quiet-but-healthy server is no
  longer judged against the previous connection's silence and torn down
  again on the very next reconnect tick; and the arming stores are now
  ordered timestamp-before-flag (release/acquire) so a freshly armed timer
  can never be observed with a stale timestamp.

## [1.2.0] - 2026-07-18

### Added
- Dispatched commands now carry a per-server operator-action identifier:
  `fss_message_asset_command.server_command_id` (the dispatching server's
  command DB row id), negotiated via the new `FSS_FEATURE_SERVER_COMMAND_ID`
  capability and carried as an optional trailing wire field, so legacy peers
  see an unchanged wire format. Within one connection, the same id means the
  same operator action (a redelivery) and a new id means a new action — a
  deliberate operator retry with identical command/payload gets a fresh id on
  every server, which is what lets an FMU connected to redundant servers tell
  a genuine retry apart from another server's copy of the same push (cap-fmu
  todo/86). Ids are unique per server only and must never be compared across
  connections; 0 means "not reported". (todo/49)
- Per-connection configurable `TCP_USER_TIMEOUT`: a flight-safety client
  (e.g. cap-fmu) can now bound how long a blocking send into a half-dead
  server can stall — `tcp_user_timeout_ms` in the client JSON config, a
  protected `fss_client::setTcpUserTimeoutMs()` for programmatic use, and
  `fss_connection::setTcpUserTimeoutMs()` /
  `fss_connection_client::create(..., t_tcp_user_timeout_ms)` at the
  transport layer. The default stays the established 30 s everywhere, and
  server-accepted sockets are unaffected. (todo/26)
- Configurable duplicate-identity handling: a second connection identifying
  as an already-connected asset is now resolved by policy instead of both
  sessions going live. `duplicate_identity_policy` in `server.json` selects
  `reject_newcomer` (default: keep the established session and refuse the
  newcomer, avoiding flip-flop on a genuine misconfiguration) or
  `evict_oldest` (favour a fast reconnect). Applied at identify, aircraft
  connections only — non-aircraft clients never claim an asset id. (todo/31)
- Client-library hooks added for the e2e harness, usable by any consumer: a
  `non_aircraft` config flag so `sendIdentify()` sends
  `identity_non_aircraft`, and `clock_offset_ms` (config field) +
  `getSkewedTimestamp()` so a client reports a deliberately skewed clock.
  (todo/28, todo/33)

### Changed
- `IDatabase::getActiveServers()` now reports a cut-short read (mid-cursor
  error, truncated row) as an explicit `std::nullopt` status instead of
  throwing `database_error` across the poller thread — the codebase's last
  use of an exception as routine cross-thread control flow. Safety no longer
  depends on every caller remembering a catch block: an unguarded future
  caller previously meant `std::terminate` on a safety-critical service.
  Behaviour on a failed read is unchanged (poller keeps the previous cached
  list; identify skips the server-list send). (todo/24)
- ABI: all four library sonames bump `.so.2` → `.so.3`
  (`-version-info 3:0:0` for `libfss`, `libfss-transport`,
  `libfss-transport-ssl`, `libfss-client-ssl`). The installed
  `fss-transport.hpp` changed object layout (`fss_connection` gained
  `tcp_user_timeout_ms`, todo/26) and vtable shape
  (`fss_message_asset_command` gained `getServerCommandId()`, todo/49) since
  the 1.1.x line; a same-soname mix of an old library with the new header
  (or vice versa) would silently corrupt derived-class layouts rather than
  fail to link. Consumers must rebuild. (todo/61)
- The optional-trailing-wire-field extension rule is now documented in
  `fss-transport.hpp`, next to the `FSS_FEATURE` flag block: any future
  optional trailing field on an already-extended message must be
  prefix-closed with the earlier ones (emitted together, 0-filled if unset)
  so length alone still decodes it unambiguously. No wire or code change —
  the two fields this already governs (`rtt_response.client_timestamp`,
  `asset_command.server_command_id`) are unaffected. (todo/51)

### Fixed
- Failed telemetry/command DB writes are now detected at all: the six ECPG
  write functions (position/RTT/status/search/dispatch/ack) ran their SQL
  and returned unconditionally, never checking `sqlca.sqlcode`, so a failing
  write — e.g. a full disk — was silent audit loss that the fail-safe
  machinery never saw (`write_failure_count()` stayed 0 through an entire
  disk-full run). They now report failure, and the `db.cpp` wrappers raise
  it into the write queue's failure accounting — the signal that arms the
  todo/45/47 fail-safe entries below. (todo/34)
- The main loop no longer makes blocking DB calls: the per-connection
  reconnect health probe (a live `SELECT 1` per tick) moved onto the
  command poller, which already owns every DB read, so a frozen or
  black-holed Postgres now degrades to "caches go stale" instead of
  "command dispatch stops fleet-wide". Defence in depth:
  `PGTCPUSERTIMEOUT=25000` bounds in-flight libpq stalls on the same 25 s
  clock as the keepalive envelope, so a sustained partition fails writes —
  tripping the fail-safe — instead of hanging the writer forever. (todo/46)
- A terminal command-ack outcome (actioned/superseded/rejected/noop) is now
  final for its dispatch: a later terminal ack — from a buggy, misordered or
  forged peer — can no longer silently rewrite a settled outcome in the
  command audit trail. The guard previously only stopped a late "received"
  from regressing a terminal state. Redelivery still re-acks normally:
  recording a dispatch reopens the row's ack cycle by clearing the ack
  columns, so the stored ack always describes the latest dispatch. (todo/48)
- A dropped command dispatch/ack DB write now trips the fail-safe (severing
  every client) instead of only logging a counter change while the server
  kept flying aircraft on an audit trail it knew was broken. (todo/45)
- The DB fail-safe now latches: a trip raises an admission gate before the
  severance, and new sessions are refused until the write queue has drained
  and stayed quiet for `db_write_failure_recovery_grace_secs`. Previously a
  write-only DB failure (reads fine, writes failing — e.g. a wedged write
  connection or an insert-only fault) flapped every aircraft in and out of
  comms-loss RTL on a ~7 s period: severed, re-identified via the working
  read path, severed again. Both fail-safe triggers (sustained write
  failure, todo/34; command drop, todo/45) share the one degraded-state
  machine (`src/server-failsafe.hpp`) and recovery gate. (todo/47)
- A single transient DB write failure no longer severs the fleet: the
  fail-safe trip now requires failures genuinely spanning
  `db_write_failure_disconnect_ticks`. The todo/34 tracker documented this
  intent but tripped anyway whenever the recovery grace exceeded the
  disconnect threshold — the default configuration.
- Duplicate-identity resolution is now atomic: `server_clients` records an
  asset-id claim under its lock at identify time (released on disconnect),
  where it previously checked a snapshot of already-published ids and let
  the caller publish later — two connections identifying the same asset at
  the same instant could each miss the other's unpublished claim and both go
  live, violating both `duplicate_identity_policy` contracts (commands
  delivered to two sessions presenting the same aircraft identity, telemetry
  from two sources stored under one asset). (todo/44)
- Four config fields that silently produce a live-looking but data-dead (or
  flapping) server at 0 now warn and substitute a safe default, matching the
  existing `tls_handshake_timeout_ms`/`max_concurrent_handshakes` guards:
  `client_timeout` and `identify_timeout` (0 would sever/prune every client
  on the very next tick) and `message_rate_capacity`/`message_rate_refill` (0
  silently drops all rate-limited telemetry forever while the aircraft still
  looks healthily connected, since `rtt_response` is exempt from rate
  limiting — todo/37). (todo/50)
- `sendRTTRequest()` now sends through the null-safe base-class `sendMsg()`,
  closing the one remaining unguarded `getConnection()` dereference among the
  three per-client send paths. Not reachable as a crash today — safe only
  because of a call-graph ordering invariant documented nowhere near the call
  site — but this hardens it the same way `sendCommand()` and
  `sendSMMSettings()` already were. (todo/53)
- `processMessage()` now captures the connection `shared_ptr` once, up
  front, and bails if teardown has already cleared it; the local reference
  keeps the connection alive and non-null for the whole call, so a
  mid-message teardown reads as failed sends on a closed fd. Previously
  several handler sites dereferenced `getConnection()` unguarded — safe in
  production only via the recv-thread join inside `disconnect()`, and a
  concurrent teardown (evict_oldest severing a client mid-identify) crashed
  the unit harness under TSan through exactly that window. (PR #332)
- `certs/revoke-client.sh` no longer hangs forever when run
  non-interactively — `certtool --generate-crl` had no `--template`, so any
  scripted invocation sat at an interactive prompt — and the example
  `fake_client` now ignores SIGPIPE the way the server does, so a send
  after the peer resets the connection fails the send instead of killing
  the process. Both surfaced by the CRL-revocation e2e test. (todo/29)

### Security
- `certs/revoke-client.sh` no longer silently un-revokes earlier
  revocations: certtool's `--generate-crl` honours only the last
  `--load-certificate` flag (observed with certtool 3.8.13), so revoking a
  second client produced a CRL containing only that client — and a server
  reloading the CRL (SIGHUP, or a fresh listener) would accept every
  previously revoked certificate again. The script now concatenates all
  revoked certs into one PEM and passes it once, a form every certtool
  version honours; regression-tested by revoking two clients and asserting
  both serials stay on the CRL. (todo/63)
- Two `secure_string` scrubbing gaps closed: the recv-side frame and the
  packed send buffer of `smm_settings` messages (SMM credentials in flight)
  are now wiped after use, and the PostgreSQL password is held in
  `secure_string` end-to-end (`pg_pass` and `db_connection`'s copy), with a
  by-value nul-terminated accessor for ECPG rather than a cached mutable
  buffer the two reconnecting DB threads could race on. (todo/43)

### Packaging
- The release tarball now ships `LICENSE.md` and `CHANGELOG.md`, and always
  includes `tests/tsan.supp` and the test support headers — automake only
  distributes conditional `EXTRA_DIST` entries when the condition is true
  in the tree that runs `make dist`, so tarballs rolled without
  `--enable-tsan` silently dropped the TSan suppression file. The dist-hook
  now also strips `e2e/.ruff_cache` and the generated `e2e/traceability.json`.

## [1.1.1] - 2026-07-06

### Fixed
- `sendCommand`/`sendRTTRequest` no longer hold `client_lock` across a
  blocking send (up to 30 s on a black-holed peer); the main loop's
  `checkTimeouts()` takes the same lock, so one stuck peer could freeze the
  100 ms command-dispatch tick fleet-wide. Completes todo/21's per-client
  writer-thread guarantee, which this partially reopened. (todo/35)
- `broadcastMsg` now routes through the per-client outbound workers (each
  recipient gets its own message clone) instead of sending inline: the 15 s
  server-list broadcast no longer blocks the main loop, and position relay no
  longer blocks the reporting client's recv thread per message. (todo/36)
- `rtt_response` and `command_ack` are now exempt from the per-client rate
  limiter (acks get their own bucket), fixing a spurious liveness reap of a
  chatty-but-healthy client and a permanent audit-link loss when an ack got
  rate-limited (acks are never resent). (todo/37)
- A message too large to frame (>64 KB) is no longer transmitted unframed
  with a zero length field — which desynced the peer's stream into a
  mass-undecodable-frame disconnect. `sendMsg()` now fails the send outright
  and rolls back the consumed message id so no sequence gap is left behind.
  (todo/38)
- A v2 sequence mismatch now disconnects the session immediately, matching
  the existing identity-mismatch behaviour, instead of silently freezing
  `expected_seq` and discarding all further telemetry until the 30 s liveness
  reap. (todo/39)
- The wire message type is now validated (via a new `decode_message_type`
  helper) before being cast to `fss_message_type`, closing the last wire enum
  that was cast straight from an untrusted byte without validation. (todo/40)
- `db_active_fss_servers_get` now fails the read when a FETCH truncates a
  server address instead of silently shipping the partial list as complete:
  the mid-cursor `database_error` guard now also fires on the
  `WHENEVER SQLWARNING DO BREAK` exit path, so the poller keeps its previous
  good cache. (todo/41)
- The PF_INET6 listener now explicitly sets `IPV6_V6ONLY=0` before `bind()`,
  so IPv4 clients are no longer silently refused on hosts with
  `net.ipv6.bindv6only=1`. (todo/42)

## [1.1.0] - 2026-06-22

### Added
- Command acknowledgements. An aircraft client (FMU) now sends a
  `command_ack` back to the server for each `asset_command`, and the server
  routes and stores it against the originating command. The ack carries the
  acked command's id (matched per-connection, not "last command seen"), the
  outcome (`received`, `actioned`, `superseded`, `noop`, `rejected`) and, when
  superseded, a dedicated reason (`low_battery`, `comms_loss`,
  `newer_command`) kept distinct from the command value because the
  low-battery and comms-loss safety latches both resolve to RTL. The ack DB
  match is scoped by asset so a `dispatch_id` collision between assets can
  never cross acks, and only the latest command row is updated. Gated by the
  `command-ack` optional capability and appended as a new message type, so
  legacy peers are unaffected. (todo/17 item 1)
- Optional-capability negotiation in the protocol version handshake. The
  handshake's `feature_flags` field is now a bitmask of capabilities each peer
  supports; the negotiated set is the intersection of the two, recorded on the
  connection. This lets new optional features be added without a
  protocol-version bump — a peer that does not advertise a capability never has
  it used, and a peer cannot enable one this build does not implement. Two
  capabilities are enabled in this release (`rtt-offset` and `command-ack`); a
  legacy peer advertises none, negotiates the empty set, and is wire-compatible
  as before.
- The position-staleness gate now corrects for a measured client↔server clock
  offset, so a client whose clock is skewed but whose link is healthy no longer
  has all its positions dropped. The offset is estimated from RTT responses: a
  peer that negotiates the `rtt-offset` capability stamps its wall clock
  into each `rtt_response`, and the server folds `client - server` (bounded by
  half the round trip, smoothed across samples) into the gate. The stored
  timestamp is never rewritten — only the accept/reject window shifts. A peer
  that does not negotiate the capability is unaffected and the gate stays a
  plain symmetric window.
- `fss_client::isConfigured()` lets a consumer detect that a client loaded a
  usable configuration (an asset name plus at least one server) and fail fast
  on a missing or malformed config file. The flag stays accurate however the
  client is built — the file constructor and the programmatic
  `setAssetName()`/`connectTo()` path both keep it current — and no-throw
  construction is preserved. (todo/18)

### Changed
- Library ABI: `-version-info` for `libfss`, `libfss-transport`,
  `libfss-transport-ssl` and `libfss-client-ssl` is bumped to `2:0:0`. This
  release changes installed-header interfaces (new `command_ack` message type
  and enums, enlarged `fss_connection`/`fss_listen`/`rtt_response`, added
  constructor parameters on the SSL transport, new `fss_client` virtuals), so
  binaries built against 1.0.x must be relinked against 1.1.0.
- Command dispatch, RTT requests and SMM-settings sends now run on a
  per-client writer thread instead of the server main loop. A slow or
  black-holed peer can no longer stall the main loop, command/RTT work for
  other clients, or unrelated server processing. (todo/21)
- `make check` now fails fast with a clear error when the tree was configured
  without `--enable-tests`, instead of silently reporting success with zero
  tests run.

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
  0 disables). The gate also carries the per-client clock-offset correction
  described under Added, so a skewed-but-healthy client's positions are
  accepted rather than dropped.
- A command dispatch is recorded only after `sendMsg()` succeeds. A failed
  send no longer leaves a false audit record of a command that never went out;
  failed sends are surfaced and retried promptly. (todo/19)
- Command dispatch and ack DB writes are protected from telemetry-queue
  overflow. A burst of telemetry can no longer cause command/ack records to be
  silently dropped from the write queue. (todo/20)
- An RTT request is tracked as outstanding only after it is successfully sent,
  so a failed send no longer creates a phantom liveness probe that never gets
  a response and skews link measurements. (todo/22)
- A peer that streams a bounded number of consecutive undecodable frames is now
  disconnected instead of being logged at line rate forever; a single
  decodable frame in between resets the counter, so a version-skewed-but-honest
  peer sending the odd unknown message type is never dropped. (todo/11)
- A truncated active-server list read mid-cursor is now discarded rather than
  returned partially; the ECPG C structures in the read getters are freed on
  the out-of-memory path; and the rate limiter's token refill stays accurate
  above 1000 tokens/s. (todo/14)
- The client's `configured` flag now has a coherent locking model, guarded
  consistently across the file-load and programmatic configuration paths.
- Configured TCP ports are validated on load. The server (`port`,
  `postgres.port`) and the client (each `servers[].port`) now reject a
  non-integer or out-of-range value (outside 1–65535) with a clear error
  instead of silently truncating it to a wrong `uint16_t`: the server refuses
  to start and the client skips the malformed server entry.
- The out-of-order / duplicate (v2 sequence) message warning is now throttled
  (first, then every 100th). Because the sequence check runs before the
  per-client rate limiter, a peer streaming wrong sequence numbers could
  otherwise flood the log at line rate; valid traffic is unaffected.
- A command row whose `command` string exceeds the read buffer is now rejected
  instead of being dispatched truncated (which could become a different or an
  unknown command). The ECPG truncation warning is checked, mirroring the
  existing guard on SMM-settings credentials.

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

[1.2.0]: https://github.com/canterbury-air-patrol/flight-safety-system/compare/1.1.1...1.2.0
[1.1.1]: https://github.com/canterbury-air-patrol/flight-safety-system/compare/1.1.0...1.1.1
[1.1.0]: https://github.com/canterbury-air-patrol/flight-safety-system/compare/1.0.3...1.1.0
[1.0.3]: https://github.com/canterbury-air-patrol/flight-safety-system/compare/1.0.2...1.0.3
[1.0.2]: https://github.com/canterbury-air-patrol/flight-safety-system/compare/1.0.1...1.0.2
[1.0.1]: https://github.com/canterbury-air-patrol/flight-safety-system/compare/1.0.0...1.0.1
[1.0.0]: https://github.com/canterbury-air-patrol/flight-safety-system/compare/0.12.3...1.0.0
[0.12.3]: https://github.com/canterbury-air-patrol/flight-safety-system/releases/tag/0.12.3
