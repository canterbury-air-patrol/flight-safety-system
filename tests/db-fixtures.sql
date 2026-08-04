-- Fixtures the live-database unit tests (db_connection_test.cpp) expect to
-- find. Apply to a database that already carries e2e/schema/001_init.sql.
--
-- Kept in one file because two harnesses need exactly the same rows: the
-- docker-compose db-unit-test runner (docker/db-unit-test-entrypoint.sh) and
-- CI's coverage job, which runs the same suite against a service container so
-- the published number includes the DB layer. Seeded once, before the suite;
-- the tests that mutate these rows restore them.

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

-- todo/80 fixture: an asset retired in fss-web (migration 0013). Seeded
-- permanently retired and never reactivated, so it is invisible to every other
-- test -- getAssetId must treat it as an unknown name, which is exactly what
-- makes it safe to leave lying in the fixture set. The reactivation case flips
-- a copy of its own making rather than this row.
INSERT INTO assets_asset (name, retired_at) VALUES ('retired-asset', NOW());
