#!/usr/bin/env bash
# Entrypoint for the db-unit-test container.
#
# Pre-populates the test database with fixtures that the DB unit tests
# expect to find, then runs the full test binary.  The Docker healthcheck
# on the db service guarantees Postgres is accepting connections before
# this script runs, so no additional wait loop is needed.
set -euo pipefail

PGPASSWORD="${TEST_DB_PASS:-password}" psql \
    -h "${TEST_DB_HOST:-db}" \
    -p "${TEST_DB_PORT:-5432}" \
    -U "${TEST_DB_USER:-postgres}" \
    -d "${TEST_DB_NAME:-postgres}" \
    <<'SQL'
INSERT INTO assets_asset (name) VALUES ('test-asset');

INSERT INTO config_smmconfig (address, port, https)
    VALUES ('smm.example.com', 80, false);

INSERT INTO config_assetconfig (asset_id, smm_id, smm_login, smm_password)
    SELECT a.id, s.id, 'testuser', 'testpass'
    FROM   assets_asset a, config_smmconfig s
    WHERE  a.name = 'test-asset';

INSERT INTO config_serverconfig (address, client_port, active, name, config_port, https)
    VALUES ('fss.example.com', 20202, true, 'test-server', 8090, false);

-- todo/41 fixture: an address of 255 two-byte UTF-8 characters (510 bytes)
-- fits VARCHAR(255) (which counts characters, not bytes) but overflows the
-- 256-byte ECPG host variable server_address is fetched into, triggering a
-- truncation warning. Seeded inactive so it is invisible to every other
-- getActiveServers() test; the todo/41 regression test flips it active for
-- the duration of that one test only.
INSERT INTO config_serverconfig (address, client_port, active, name, config_port, https)
    VALUES (repeat('é', 255), 20203, false, 'test-server-truncated', 8091, false);

-- A pending command for test-asset, so the getCommand DB test finds a row.
INSERT INTO assets_assetcommand (asset_id, command)
    SELECT a.id, 'RTL' FROM assets_asset a WHERE a.name = 'test-asset';
SQL

exec /code/tests/all_test "$@"
