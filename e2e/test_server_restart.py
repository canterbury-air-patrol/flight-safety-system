"""Client should reconnect after the server bounces."""
from __future__ import annotations

import os
import signal
import socket
import subprocess
import time
from pathlib import Path
from typing import Iterator, IO

import psycopg2
import pytest

from conftest import (
    E2E_ROOT,
    REPO_ROOT,
    SERVER_BIN,
    _pick_port,
    _render_template,
    _wait_for,
    wait_for_row,
)


@pytest.mark.requires_docker
@pytest.mark.slow
def test_client_reconnects_after_server_bounce(
    migrated_db, certs_dir, tmp_path, db_conn, reset_db,
):
    """Start server on a fixed port; connect fake-client; SIGTERM server;
    spawn a new server on the *same* port; the fake-client should reconnect
    and resume persisting position rows."""

    with psycopg2.connect(
        host=migrated_db["host"], port=migrated_db["port"],
        user=migrated_db["user"], password=migrated_db["password"],
        dbname=migrated_db["dbname"],
    ) as setup_conn:
        setup_conn.autocommit = True
        with setup_conn.cursor() as cur:
            cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
            asset_id = cur.fetchone()[0]

    port = _pick_port()
    config_path = tmp_path / "server.json"
    _render_template(
        E2E_ROOT / "fixtures" / "server.json.tmpl",
        config_path,
        SERVER_PORT=str(port),
        DB_HOST=migrated_db["host"],
        DB_USER=migrated_db["user"],
        DB_PASS=migrated_db["password"],
        DB_NAME=migrated_db["dbname"],
        CERTS_DIR=str(certs_dir),
    )
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(REPO_ROOT / "src" / ".libs")
    env["PGPORT"] = str(migrated_db["port"])
    env["PGPASSWORD"] = str(migrated_db["password"])

    def spawn_server(log_name: str) -> tuple[subprocess.Popen, IO[bytes]]:
        log_fp = (tmp_path / log_name).open("wb")
        p = subprocess.Popen(
            [str(SERVER_BIN), str(config_path)],
            cwd=str(REPO_ROOT), env=env,
            stdout=log_fp, stderr=subprocess.STDOUT,
        )
        def listening():
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(0.2)
                try:
                    s.connect(("127.0.0.1", port))
                    return True
                except OSError:
                    return False
        assert _wait_for(listening, 10.0), f"server {log_name} never bound"
        # The readiness probe opens a raw TCP connection that triggers a
        # failed TLS handshake on the server's accept thread, blocking it
        # briefly.  Give the thread a moment to recover before any real
        # client connects.
        time.sleep(0.5)
        return p, log_fp

    server1, server1_log = spawn_server("server1.log")

    # Use the fake_client factory, which was configured against server_proc.
    # Override its SERVER_PORT by using a fresh render path.
    # (Instead of the factory, spawn fake-client manually so we can point it
    # at the port we chose.)
    from conftest import FAKE_CLIENT_BIN
    client_cfg = tmp_path / "client-test1.json"
    _render_template(
        E2E_ROOT / "fixtures" / "client.json.tmpl",
        client_cfg,
        CLIENT_NAME="test1",
        CERTS_DIR=str(certs_dir),
        SERVER_PORT=str(port),
    )
    client_log = tmp_path / "client-test1.log"
    client_fp = client_log.open("wb")
    client = subprocess.Popen(
        [str(FAKE_CLIENT_BIN), str(client_cfg)],
        cwd=str(REPO_ROOT), env=env,
        stdout=client_fp, stderr=subprocess.STDOUT,
    )

    try:
        conn = psycopg2.connect(
            host=migrated_db["host"], port=migrated_db["port"],
            user=migrated_db["user"], password=migrated_db["password"],
            dbname=migrated_db["dbname"],
        )
        conn.autocommit = True

        # Wait for first position row. fake-client send cadence is 5s and
        # SSL handshake can take a beat, so give it up to 25s.
        first = wait_for_row(
            conn,
            "SELECT id FROM assets_assetposition WHERE asset_id = %s "
            "ORDER BY timestamp LIMIT 1",
            params=(asset_id,),
            timeout=25.0,
        )
        if first is None:
            raise AssertionError(
                "no position row before bounce\n"
                f"-- server1 log --\n{(tmp_path / 'server1.log').read_text(errors='replace')}\n"
                f"-- client log --\n{client_log.read_text(errors='replace')}"
            )
        first_id = first[0]

        # Bounce the server.
        server1.send_signal(signal.SIGINT)
        try:
            server1.wait(timeout=15)
        except subprocess.TimeoutExpired:
            server1.kill()
            server1.wait(timeout=5)
        time.sleep(1.0)
        server2, server2_log = spawn_server("server2.log")

        # The client should reconnect (retry_delay starts at 1000ms) and push
        # a new position row within the next reconnect window + 5s send cadence.
        second = wait_for_row(
            conn,
            "SELECT id FROM assets_assetposition WHERE asset_id = %s AND id > %s "
            "ORDER BY id LIMIT 1",
            params=(asset_id, first_id),
            timeout=30.0,
        )
        assert second is not None, (
            "client did not reconnect / resend position within 30s\n"
            + client_log.read_text(errors="replace")
        )
    finally:
        if client.poll() is None:
            client.send_signal(signal.SIGINT)
            try: client.wait(timeout=5)
            except subprocess.TimeoutExpired:
                client.kill(); client.wait(timeout=5)
        client_fp.close()
        for s in (server1, server2 if 'server2' in locals() else None):
            if s is not None and s.poll() is None:
                s.send_signal(signal.SIGINT)
                try: s.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    s.kill(); s.wait(timeout=5)
        for l in (server1_log, server2_log):
            if l is not None:
                l.close()
