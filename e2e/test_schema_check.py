"""The server must refuse to start against an un-migrated database (todo/73).

The tables FSS reads and writes belong to fss-web, a separate repository, and
are created by its Django migrations. If a deployment's fss-web has not applied
the migration carrying the command-ack columns, the server starts cleanly,
accepts every aircraft, and then severs the whole fleet on the first command
dispatch: recordCommandDispatch throws, the write queue counts the failure, and
the DB fail-safe latches -- permanently, because the schema is still wrong.

The remedy is to fail at startup instead, so this asserts the three things an
operator depends on when it fires: the process exits non-zero, the log names the
column that is missing, and no listener is left behind for an aircraft to
connect to.

Parametrised over every column that gates startup for a *safety* reason, so
adding one to required_columns[] means adding a case here. The second is
assets_asset.retired_at (fss-web migration 0013, todo/80): an FSS release that
enforces retirement must not run against a schema that cannot express it, or a
retired aircraft keeps flying. The refusal is the intended behaviour on an
upgrade done in the wrong order, which is why the release note says so.

The unit suite covers the same contract at the db_connection seam
(db_connection_test.cpp); what only a real process can show is the exit status
and the log.
"""
from __future__ import annotations

import os
import signal
import socket
import subprocess

import psycopg2
import pytest

from conftest import (
    E2E_ROOT,
    REPO_ROOT,
    SERVER_BIN,
    _pick_port,
    _render_template,
)

# The check is one information_schema query on an already-open connection, so
# the refusal is effectively immediate. Anything approaching this bound means
# the server got past the gate and is doing something else.
REFUSAL_TIMEOUT_S = 15


# Each case is (table, column, drop_sql, restore_sql). Every entry must restore
# the table *exactly*, indexes included -- see the finally block below for why.
GATED_COLUMNS = [
    pytest.param(
        "assets_assetcommand",
        "ack_state",
        "ALTER TABLE assets_assetcommand DROP COLUMN ack_state",
        "ALTER TABLE assets_assetcommand ADD COLUMN ack_state SMALLINT",
        id="ack_state-migration-0008",
    ),
    pytest.param(
        # todo/80. Dropping the column drops its index with it, so the restore
        # re-creates both; the name matches schema/001_init.sql so a later
        # re-apply of that file behaves identically.
        "assets_asset",
        "retired_at",
        "ALTER TABLE assets_asset DROP COLUMN retired_at",
        "ALTER TABLE assets_asset ADD COLUMN retired_at TIMESTAMP WITH TIME ZONE; "
        "CREATE INDEX assets_asset_retired_at_idx ON assets_asset (retired_at)",
        id="retired_at-migration-0013",
    ),
]


@pytest.mark.satisfies("TC-SRV-007")
@pytest.mark.requires_docker
@pytest.mark.parametrize(("table", "column", "drop_sql", "restore_sql"), GATED_COLUMNS)
def test_server_refuses_to_start_without_a_required_column(
    certs_dir, tmp_path, migrated_db, table, column, drop_sql, restore_sql
):
    """Drop a required column and assert fss-server fail-stops naming it.

    Both columns here are nullable with no default or constraint, so dropping
    and re-adding restores the table exactly.

    The restore in the finally block is mandatory, not tidiness: in CI the
    database is a shared long-lived service (FSS_E2E_EXTERNAL_DB), tests run
    serially in one session, and schema/001_init.sql is not idempotent -- a
    column left dropped would poison every later test.

    Spawns the binary directly rather than through the `server_proc` fixture,
    which raises when the port never opens. Not opening the port is the point.
    """
    if not SERVER_BIN.exists():
        pytest.skip(f"fss-server not built at {SERVER_BIN}")

    conn = psycopg2.connect(
        host=migrated_db["host"],
        port=migrated_db["port"],
        user=migrated_db["user"],
        password=migrated_db["password"],
        dbname=migrated_db["dbname"],
    )
    conn.autocommit = True
    try:
        with conn.cursor() as cur:
            cur.execute(drop_sql)

        port = _pick_port()
        config_path = tmp_path / "server-bad-schema.json"
        log_path = tmp_path / "server-bad-schema.log"
        _render_template(
            E2E_ROOT / "fixtures" / "server.json.tmpl",
            config_path,
            SERVER_PORT=str(port),
            DB_HOST=str(migrated_db["host"]),
            DB_PORT=str(migrated_db["port"]),
            DB_USER=str(migrated_db["user"]),
            DB_PASS=str(migrated_db["password"]),
            DB_NAME=str(migrated_db["dbname"]),
            CERTS_DIR=str(certs_dir),
        )

        env = os.environ.copy()
        env["LD_LIBRARY_PATH"] = str(REPO_ROOT / "src" / ".libs")

        with log_path.open("wb") as log_fp:
            # sourcery skip: dangerous-subprocess-use-audit
            proc = subprocess.Popen(
                [str(SERVER_BIN), str(config_path)],
                cwd=str(REPO_ROOT),
                env=env,
                stdout=log_fp,
                stderr=subprocess.STDOUT,
            )
            try:
                rc = proc.wait(timeout=REFUSAL_TIMEOUT_S)
            except subprocess.TimeoutExpired:
                proc.send_signal(signal.SIGINT)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
                pytest.fail(
                    f"fss-server did not exit within {REFUSAL_TIMEOUT_S}s with "
                    f"{table}.{column} missing; log:\n{log_path.read_text()}"
                )

        log = log_path.read_text()
        assert rc != 0, (
            f"fss-server exited 0 against a database missing {table}.{column} "
            f"(expected non-zero); log:\n{log}"
        )
        # The message must name the column: "schema check failed" alone leaves
        # the operator to go and find which migration is missing.
        assert column in log, (
            f"startup refusal did not name the missing column {column}; log:\n{log}"
        )

        # Nothing may be left listening — an aircraft must not be able to reach
        # a server that has refused to run.
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(0.2)
            with pytest.raises(ConnectionRefusedError):
                s.connect(("127.0.0.1", port))
    finally:
        with conn.cursor() as cur:
            cur.execute(restore_sql)
        conn.close()
