"""End-to-end ECPG negative paths.

Covers the gaps from todo/test-coverage-gaps.md Phase 2 that are not already
exercised by the round-trip tests (test_position_flow.py, test_rtt.py):

- The server must fail (exit non-zero) when configured with an unreachable
  Postgres host. This is the B5.2 happy-shutdown contract: an unconnectable
  DB is a fatal startup error, not a silent stall.
- A client whose CN authenticates against the trusted CA but is not enrolled
  in assets_asset must be rejected at identify; no rows may land for that
  asset id.
"""
from __future__ import annotations

import os
import signal
import socket
import subprocess
import time

import pytest

from conftest import (
    E2E_ROOT,
    REPO_ROOT,
    SERVER_BIN,
    _pick_port,
    _render_template,
)


# RFC-5737 documentation block — guaranteed not to route to a real Postgres.
UNREACHABLE_DB_HOST = "192.0.2.1"


@pytest.mark.requires_docker
def test_server_exits_when_db_host_invalid(certs_dir, tmp_path, migrated_db):
    """fss-server must fail-stop when its configured DB host is unreachable.

    We render a server config pointing at an RFC-5737 black-hole address and
    spawn fss-server directly (the `server_proc` fixture would block waiting
    for a listening socket that never appears). The process must exit with
    non-zero status within DB_CONNECT_TIMEOUT_S; if it lingers, that's a
    bug — the server should not enter the accept loop without a working DB.
    """
    if not SERVER_BIN.exists():
        pytest.skip(f"fss-server not built at {SERVER_BIN}")

    port = _pick_port()
    config_path = tmp_path / "server-bad-db.json"
    log_path = tmp_path / "server-bad-db.log"
    _render_template(
        E2E_ROOT / "fixtures" / "server.json.tmpl",
        config_path,
        SERVER_PORT=str(port),
        DB_HOST=UNREACHABLE_DB_HOST,
        DB_USER=migrated_db["user"],
        DB_PASS=migrated_db["password"],
        DB_NAME=migrated_db["dbname"],
        CERTS_DIR=str(certs_dir),
    )

    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(REPO_ROOT / "src" / ".libs")

    log_fp = log_path.open("wb")
    # sourcery skip: dangerous-subprocess-use-audit
    proc = subprocess.Popen(
        [str(SERVER_BIN), str(config_path)],
        cwd=str(REPO_ROOT),
        env=env,
        stdout=log_fp,
        stderr=subprocess.STDOUT,
    )

    # libpq's default connect timeout is long; give it ~25 s to give up.
    DB_CONNECT_TIMEOUT_S = 25
    try:
        rc = proc.wait(timeout=DB_CONNECT_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        log_fp.close()
        pytest.fail(
            f"fss-server did not exit within {DB_CONNECT_TIMEOUT_S}s when "
            f"pointed at unreachable DB host; log:\n{log_path.read_text()}"
        )
    log_fp.close()

    assert rc != 0, (
        f"fss-server exited 0 with unreachable DB host (expected non-zero); "
        f"log:\n{log_path.read_text()}"
    )

    # Sanity: the chosen port should not be left listening after exit.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(0.2)
        with pytest.raises(OSError):
            s.connect(("127.0.0.1", port))


@pytest.mark.requires_docker
def test_unknown_asset_name_persists_no_data(db_conn, fake_client):
    """A client whose CN is signed by the trusted CA but not enrolled as an
    asset must be rejected at identify, and no telemetry rows may land.

    The certs_dir fixture pre-generates client certs named test1/test2/test3.
    None of them are inserted into assets_asset, so any of them will
    authenticate but fail the DB asset lookup. We use 'test1' here.

    Mirrors test_cert_rejection.py's strategy: the contract under test is
    purely behavioral (no rows persisted). Log-content checks are avoided
    because they couple the test to the exact phrasing of an error message.
    """
    # Deliberately do NOT insert into assets_asset — the asset is unknown.
    fake_client("test1")

    # Give the client time to handshake, send identity, get rejected, and
    # potentially retry a few times.
    time.sleep(12)

    # No rows should have landed for ANY asset, since no asset row exists.
    with db_conn.cursor() as cur:
        cur.execute("SELECT COUNT(*) FROM assets_assetposition")
        pos_count = cur.fetchone()[0]
        cur.execute("SELECT COUNT(*) FROM assets_assetstatus")
        status_count = cur.fetchone()[0]

    assert pos_count == 0, (
        f"expected 0 position rows for unknown asset, got {pos_count}"
    )
    assert status_count == 0, (
        f"expected 0 status rows for unknown asset, got {status_count}"
    )
