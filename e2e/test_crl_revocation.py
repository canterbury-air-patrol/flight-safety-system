"""Revoked client is disconnected on SIGHUP; others are unaffected.

Covers todo/29: revoke -> CRL regen -> SIGHUP -> disconnect + refused
reconnect, second client unaffected. Gives Tier-2 evidence for the
mechanism Tier-3 Path F re-checks with a real cap-fmu (HZ-09 mitigation).

`certs_dir` is session-scoped and shared by every other e2e test; CRL
revocation mutates the PKI (writes crl.pem + revoked/), so this file works
against its own function-scoped copy instead. The copy also needs an
initial valid (empty) CRL before the server starts, since a configured
`crl_file` that doesn't exist yet fails to load at listener construction
(`fss_connection::loadCrl()`, src/transport-ssl.cpp:232-247) -- so this test
renders its own server config (`server-crl.json.tmpl`) rather than reusing
the shared `server_proc` fixture, which has no crl_file wiring at all.
"""
from __future__ import annotations

import os
import shutil
import signal
import socket
import subprocess
import time
from pathlib import Path

import pytest

from conftest import (
    CERT_SCRIPTS,
    DEFAULT_LEARNED_EXPIRY_MS,
    E2E_ROOT,
    FAKE_CLIENT_BIN,
    REPO_ROOT,
    SERVER_BIN,
    _pick_port,
    _render_template,
    _wait_for,
    wait_for_row,
)


@pytest.fixture
def crl_certs_dir(tmp_path_factory: pytest.TempPathFactory, certs_dir: Path) -> Path:
    """Function-scoped copy of the session cert store plus an initial empty CRL."""
    target = tmp_path_factory.mktemp("certs-crl")
    for item in certs_dir.iterdir():
        if item.is_file():
            shutil.copy(item, target / item.name)
    for script in ("generate-ca.sh", "generate-server.sh", "generate-client.sh", "revoke-client.sh"):
        src = CERT_SCRIPTS / script
        if src.exists():
            shutil.copy(src, target / script)
            (target / script).chmod(0o755)
    # certtool prompts interactively for "next CRL update in (days)" unless a
    # --template supplies crl_next_update/crl_number -- without it this hangs
    # forever reading EOF from stdin under subprocess (found the hard way).
    crl_tmpl = target / "crl.tmpl"
    crl_tmpl.write_text("crl_next_update = 3650\ncrl_number = 1\n")
    subprocess.check_call(
        [
            "certtool", "--generate-crl",
            "--load-ca-privkey", "ca.private.pem",
            "--load-ca-certificate", "ca.public.pem",
            "--template", "crl.tmpl",
            "--outfile", "crl.pem",
        ],
        cwd=str(target),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return target


def test_second_revocation_keeps_the_first_on_the_crl(crl_certs_dir: Path) -> None:
    """Revoking a second client must not silently un-revoke the first.

    todo/63: certtool --generate-crl honours only the last --load-certificate
    flag (observed with certtool 3.8.13), so the script's old
    one-flag-per-revoked-cert form produced a CRL holding only the newest
    revocation -- a server reloading it (SIGHUP, or a fresh listener) would
    re-admit every client revoked earlier. No server needed here: the defect
    is in the CRL contents themselves.
    """
    for name in ("test1", "test2"):
        subprocess.check_call(
            ["./revoke-client.sh", name],
            cwd=str(crl_certs_dir),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
    info = subprocess.check_output(
        ["certtool", "--crl-info", "--infile", "crl.pem"],
        cwd=str(crl_certs_dir), stdin=subprocess.DEVNULL, text=True,
    )
    assert "Revoked certificates (2):" in info, info
    assert info.count("Serial Number") == 2, info


@pytest.mark.requires_docker
@pytest.mark.slow
def test_revocation_disconnects_and_blocks_reconnect_but_not_others(
    migrated_db, crl_certs_dir, tmp_path, db_conn, reset_db,
):
    """Revoke client A's cert, SIGHUP the server: A is disconnected and can't
    reconnect; client B (untouched cert) keeps working throughout."""
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_a = cur.fetchone()[0]
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test2') RETURNING id")
        asset_b = cur.fetchone()[0]

    port = _pick_port()
    config_path = tmp_path / "server.json"
    log_path = tmp_path / "server.log"
    _render_template(
        E2E_ROOT / "fixtures" / "server-crl.json.tmpl",
        config_path,
        SERVER_PORT=str(port),
        DB_HOST=migrated_db["host"],
        DB_PORT=str(migrated_db["port"]),
        DB_USER=migrated_db["user"],
        DB_PASS=migrated_db["password"],
        DB_NAME=migrated_db["dbname"],
        CERTS_DIR=str(crl_certs_dir),
        CRL_FILE=str(crl_certs_dir / "crl.pem"),
    )

    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(REPO_ROOT / "src" / ".libs")
    env["PGPORT"] = str(migrated_db["port"])
    env["PGPASSWORD"] = str(migrated_db["password"])

    server_log_fp = log_path.open("wb")
    # sourcery skip: dangerous-subprocess-use-audit
    server = subprocess.Popen(
        [str(SERVER_BIN), str(config_path)],
        cwd=str(REPO_ROOT), env=env,
        stdout=server_log_fp, stderr=subprocess.STDOUT,
    )

    def listening() -> bool:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(0.2)
            try:
                s.connect(("127.0.0.1", port))
                return True
            except OSError:
                return False

    def spawn_client(name: str) -> tuple[subprocess.Popen, Path]:
        client_cfg = tmp_path / f"client-{name}.json"
        client_log = tmp_path / f"client-{name}.log"
        _render_template(
            E2E_ROOT / "fixtures" / "client.json.tmpl",
            client_cfg,
            CLIENT_NAME=name,
            CERTS_DIR=str(crl_certs_dir),
            SERVER_PORT=str(port),
            LEARNED_EXPIRY_MS=str(DEFAULT_LEARNED_EXPIRY_MS),
        )
        fp = client_log.open("wb")
        # sourcery skip: dangerous-subprocess-use-audit
        p = subprocess.Popen(
            [str(FAKE_CLIENT_BIN), str(client_cfg)],
            cwd=str(REPO_ROOT), env=env,
            stdout=fp, stderr=subprocess.STDOUT,
        )
        return p, client_log, fp

    client_a = client_a_log = client_a_fp = None
    client_b = client_b_log = client_b_fp = None

    try:
        assert _wait_for(listening, timeout=10.0), (
            f"server did not begin listening in 10s:\n{log_path.read_text(errors='replace')}"
        )
        # See server_proc's own comment: the readiness probe's failed TLS
        # handshake blocks the accept thread briefly.
        time.sleep(0.5)

        client_a, client_a_log, client_a_fp = spawn_client("test1")
        client_b, client_b_log, client_b_fp = spawn_client("test2")

        conn = db_conn

        first_a = wait_for_row(
            conn,
            "SELECT id FROM assets_assetposition WHERE asset_id = %s ORDER BY id LIMIT 1",
            params=(asset_a,), timeout=25.0,
        )
        assert first_a is not None, (
            "client A never reported a position before revocation\n"
            + client_a_log.read_text(errors="replace")
        )
        first_b = wait_for_row(
            conn,
            "SELECT id FROM assets_assetposition WHERE asset_id = %s ORDER BY id LIMIT 1",
            params=(asset_b,), timeout=25.0,
        )
        assert first_b is not None, (
            "client B never reported a position before revocation\n"
            + client_b_log.read_text(errors="replace")
        )

        # Revoke A, regenerate the CRL, SIGHUP the server.
        subprocess.check_call(
            ["./revoke-client.sh", "test1"],
            cwd=str(crl_certs_dir),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        server.send_signal(signal.SIGHUP)

        # A must be disconnected: its process stays alive (auto-reconnect
        # loop) but its handshake now fails every retry, so no new position
        # rows should land for it after a window covering the reload +
        # several reconnect attempts.
        time.sleep(15.0)

        with conn.cursor() as cur:
            cur.execute(
                "SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s AND id > %s",
                (asset_a, first_a[0]),
            )
            a_rows_after = cur.fetchone()[0]
        assert a_rows_after == 0, (
            f"expected no new position rows for revoked client A, got {a_rows_after}\n"
            + client_a_log.read_text(errors="replace")
        )
        assert client_a.poll() is None, (
            "revoked client A process should not crash, just fail to reconnect\n"
            + client_a_log.read_text(errors="replace")
        )

        # B must be unaffected throughout: still connected, still storing.
        second_b = wait_for_row(
            conn,
            "SELECT id FROM assets_assetposition WHERE asset_id = %s AND id > %s ORDER BY id LIMIT 1",
            params=(asset_b, first_b[0]), timeout=15.0,
        )
        assert second_b is not None, (
            "unrevoked client B should keep reporting positions after the CRL reload\n"
            + client_b_log.read_text(errors="replace")
        )
        assert client_b.poll() is None, "client B process should not have been disconnected"
    finally:
        for proc, fp in ((client_a, client_a_fp), (client_b, client_b_fp)):
            if proc is not None:
                if proc.poll() is None:
                    proc.send_signal(signal.SIGINT)
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait(timeout=5)
                if fp is not None:
                    fp.close()
        if server.poll() is None:
            server.send_signal(signal.SIGINT)
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=5)
        server_log_fp.close()
