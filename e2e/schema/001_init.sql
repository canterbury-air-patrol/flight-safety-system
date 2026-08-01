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

-- position is nullable and gps_fix_valid exists as of fss-web migration 0016:
-- a report whose coordinates are not GPS-backed is still stored (the autopilot's
-- dead-reckoned estimate is worth showing, labelled), and a report carrying no
-- coordinates at all stores NULL geometry rather than being discarded.
CREATE TABLE assets_assetposition (
    id             BIGSERIAL PRIMARY KEY,
    asset_id       BIGINT NOT NULL REFERENCES assets_asset(id),
    position       GEOMETRY(POINT, 4326),
    altitude       INTEGER NOT NULL,
    gps_fix_valid  BOOLEAN NOT NULL DEFAULT TRUE,
    timestamp      TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW()
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
    timestamp   TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
    -- Command-acknowledgement columns (mirror fss-web migration 0008; the
    -- server writes these, see todo/17 item 1). dispatch_id is the stamped
    -- per-connection message id; the ack_* fields are filled when the asset
    -- acks. All nullable: a command with no ack yet leaves them NULL.
    dispatch_id       BIGINT,
    ack_state         SMALLINT,
    ack_timestamp     BIGINT,
    ack_superseded_by SMALLINT
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
    active       BOOLEAN NOT NULL DEFAULT TRUE,
    name         VARCHAR(25)  NOT NULL DEFAULT '',
    config_port  INTEGER      NOT NULL DEFAULT 8090,
    https        BOOLEAN      NOT NULL DEFAULT FALSE
);
