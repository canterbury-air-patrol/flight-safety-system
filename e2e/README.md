# E2E test harness

Exercises the real `fss-server` binary against a real PostgreSQL+PostGIS
database, with real fake-client processes driving traffic over TLS.

## Prerequisites

- `docker` daemon reachable (pulls `postgis/postgis:18-3.6-alpine` on first run)
- Python 3.9+
- The project built with `--enable-server --enable-fake-client`:

  ```sh
  ./autogen.sh
  ./configure --enable-tests --enable-server --enable-fake-client
  make -j
  ```

## Running

```sh
cd e2e
pip install -r requirements.txt
pytest
```

Or from the repo root:

```sh
make e2e-test
```

`make e2e-test` is opt-in and is **not** part of `make check`.

## How it works

- `pg_container` (session-scoped) boots a PostGIS container on a random port
  and exposes its connection string.
- `migrated_db` applies everything under `schema/*.sql` in lexical order. The
  bundled `schema/001_init.sql` is a minimal hand-authored schema covering
  only the columns the server binary touches (see `src/server-db.pgc`).
- `certs_dir` (session-scoped) runs the project's `certs/generate-*.sh`
  scripts into a per-session temp dir so each test run gets a fresh PKI.
- `server_proc` renders `fixtures/server.json.tmpl` with the DB + cert
  parameters, spawns `src/fss-server`, waits for the port to accept
  connections, and tears the server down with SIGINT on teardown.
- `fake_client` is a factory that spawns `examples/fss-fake-client` against
  the active server. Tests call it per scenario.

## Schema drift

`schema/001_init.sql` is hand-written and covers exactly what
`src/server-db.pgc` reads and writes. The production Django schema
(`fss-web`) defines these tables via migrations and may add columns over
time. If the server starts referencing a new column, this file needs a
matching edit.

The plan's **primary schema provisioning path** — running the `fss-web`
Django container's `manage.py migrate` against the same Postgres — is not
implemented here because `fss-web` is a separate repo. To switch over:

1. Add an `fss-web` fixture that runs `docker run --rm -e DB_HOST=... fss-web
   python manage.py migrate --noinput` against `pg_container`.
2. Remove `schema/001_init.sql` (or keep as a fallback).

## Writing new tests

- Every test using `server_proc` / `fake_client` gets the `reset_db` fixture
  transitively, which TRUNCATEs the FSS tables before the test runs.
- Tests should be idempotent — do not depend on row IDs or auto-increment
  values from a previous test.
- Wall-clock waits are unavoidable because the fake-client's send cadence is
  5s and the server's RTT cadence is 10s. Use `conftest.wait_for_row` to
  poll the DB rather than bare `time.sleep` wherever possible.
