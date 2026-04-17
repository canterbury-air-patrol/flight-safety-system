-- Minimal schema covering the columns src/server-db.pgc actually reads and
-- writes. This is NOT the full fss-web Django schema; it is only what the
-- server binary needs to start, accept messages, and exercise the code paths
-- we want to test.
--
-- When fss-web changes, this file may drift. See e2e/README.md for how to
-- refresh it from a real migrated database.

CREATE EXTENSION IF NOT EXISTS postgis;

CREATE TABLE assets_asset (
    id          BIGSERIAL PRIMARY KEY,
    name        VARCHAR(64) NOT NULL UNIQUE
);

CREATE TABLE assets_assetposition (
    id          BIGSERIAL PRIMARY KEY,
    asset_id    BIGINT NOT NULL REFERENCES assets_asset(id),
    position    GEOMETRY(POINT, 4326) NOT NULL,
    altitude    INTEGER NOT NULL,
    timestamp   TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW()
);

CREATE TABLE assets_assetrtt (
    id          BIGSERIAL PRIMARY KEY,
    asset_id    BIGINT NOT NULL REFERENCES assets_asset(id),
    rtt         BIGINT NOT NULL,
    timestamp   TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW()
);

CREATE TABLE assets_assetstatus (
    id           BIGSERIAL PRIMARY KEY,
    asset_id     BIGINT NOT NULL REFERENCES assets_asset(id),
    bat_percent  SMALLINT NOT NULL,
    bat_used_mah BIGINT NOT NULL,
    bat_volt     DOUBLE PRECISION NOT NULL,
    timestamp    TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW()
);

CREATE TABLE assets_assetsearchprogress (
    id                   BIGSERIAL PRIMARY KEY,
    asset_id             BIGINT NOT NULL REFERENCES assets_asset(id),
    search               BIGINT NOT NULL,
    search_progress      BIGINT NOT NULL,
    search_progress_of   BIGINT NOT NULL,
    timestamp            TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW()
);

CREATE TABLE assets_assetcommand (
    id          BIGSERIAL PRIMARY KEY,
    asset_id    BIGINT NOT NULL REFERENCES assets_asset(id),
    command     VARCHAR(8) NOT NULL,
    position    GEOGRAPHY(POINT, 4326),
    altitude    INTEGER,
    timestamp   TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW()
);

CREATE TABLE config_smmconfig (
    id       BIGSERIAL PRIMARY KEY,
    address  VARCHAR(255) NOT NULL,
    port     INTEGER NOT NULL,
    https    BOOLEAN NOT NULL DEFAULT FALSE
);

CREATE TABLE config_assetconfig (
    id            BIGSERIAL PRIMARY KEY,
    asset_id      BIGINT NOT NULL REFERENCES assets_asset(id) UNIQUE,
    smm_id        BIGINT REFERENCES config_smmconfig(id),
    smm_login     VARCHAR(50),
    smm_password  VARCHAR(255)
);

CREATE TABLE config_serverconfig (
    id           BIGSERIAL PRIMARY KEY,
    address      VARCHAR(255) NOT NULL,
    client_port  INTEGER NOT NULL,
    active       BOOLEAN NOT NULL DEFAULT TRUE
);
