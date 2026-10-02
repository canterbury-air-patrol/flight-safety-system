# Duplicate identity audit events (todo/77)

FSS records each actual competing identity resolution in fss-web's
`assets_assetidentityevent` table (migration `0017_assetidentityevent`).
Deploy that migration before upgrading FSS: startup schema verification refuses
an incomplete database. No aircraft protocol change is needed. The transport
library API changes, so the library sonames advance from 4 to 5 and consumers
must rebuild.

## Capture and storage

The atomic claim decision is unchanged. Under the identity lock, capture the
asset ID, occurrence time in Unix milliseconds, actual outcome
(`newcomer_rejected` or `incumbent_evicted`), and both sessions' immutable
handshake evidence. Shutdown/timeout refusals and stale-owner invariant errors
are not competing live identities and produce no event. Enqueue outside the
lock, before initiating eviction. Database I/O stays on the existing writer.

Each event has a fresh version-4 UUID. A session has its own version-4 UUID,
so cloned certificates remain distinguishable as connections. Evidence also
includes the leaf certificate CN and SHA-256 fingerprint and the numeric peer
address/port when available. Values are bounded to 512 bytes (therefore at most
512 characters) and JSON-escaped; no private keys or certificate PEM is stored.
Transport evidence is captured during handshake, before receive-thread startup,
and the session snapshots it before activation, so teardown cannot erase or
race the audit snapshot. UUID generation precedes claim mutation.

The SQL binds all values and uses `ON CONFLICT (event_id) DO NOTHING`.
Occurrence time is supplied by FSS; receipt time uses the database default.
Acknowledgement columns are omitted and never updated by FSS, even if the same
event is written again. An eviction warning is immediately eligible for the
web app's next normal polling response once the async insert finishes. There
is no additional command block, push notification, or automatic policy change.

## Bounded queue and loss reporting

Identity events are protected audit tasks, separate from command writes and
loss-tolerant telemetry. At capacity, incoming events evict queued telemetry.
Neither telemetry nor command pressure can evict an accepted identity event.
If all slots already hold protected tasks, the new identity event is refused;
command overload still drops a command if one is queued, otherwise it refuses
the incoming command. Memory remains bounded during reconnect storms, and
identity resolution never waits for database capacity.

Every refused event (including enqueue after stop) and every failed event
write emits an unthrottled `IDENTITY EVENT NOT STORED` ERROR containing the
UUID, asset, occurrence time, outcome, and both JSON evidence objects. Each
increments the separate `identity_lost_count()` counter. Database exceptions
also increment the existing write-failure counter and participate in the
existing database fail-safe. A connection failure can leave the commit outcome
uncertain; the UUID supports a later idempotent replay without resetting an
operator's acknowledgement. No automatic retry or replay is implemented.

This is an explicit bounded-memory/loss-reporting policy, **not crash-durable
storage**: an abrupt process/host failure can lose queued events without a
final log. Orderly queue shutdown drains accepted events. Logs must be collected
and monitored to investigate failed persistence; a failed insert cannot itself
create a web warning. Durable spooling/replay would require a separate disk
capacity, recovery, and deployment contract.

## Policy and acceptance

`reject_newcomer` remains the default. It preserves the incumbent session;
`evict_oldest` transfers the claim to the newest connection and disconnects the
incumbent. The deployment owner must record the chosen policy and rationale
beside its `server.json`; this implementation does not select CAP's fleet policy.

Unit tests cover concurrent decisions under both policies, distinguishable
sessions, non-duplicate refusals, telemetry/command pressure, stopped queues,
per-event write-failure reporting, and retry safety for acknowledgements. TLS
and PostgreSQL integration tests assert stored evidence and command routing
under both policies, plus startup rejection when migration columns are missing.
Full TC-MAV-015 acceptance still needs real competing CAP FMUs and observation
of the deployed web warning; a server-only test is not that system acceptance.
