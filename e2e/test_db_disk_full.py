"""e2e: database disk exhaustion fails visibly, not silently (Path M m05
server share, todo/34).

Two confirmed defects, both fixed alongside this test:

1. The deeper one: src/server-db.pgc's six write functions
   (db_position_create_entry and siblings) executed their EXEC SQL
   INSERT/UPDATE and returned void unconditionally -- never checking
   sqlca.sqlcode. A failed write just got ECPG's default sqlprint() (a bare
   stderr line, no exception), while db.cpp's C++ wrapper returned
   normally as if it had succeeded. db_write_queue::run()'s catch block
   (the mechanism this todo originally assumed was the whole story) can
   only count a failure that throws -- and nothing ever did, so
   write_failure_count() stayed at 0 through an entire live run against a
   genuinely full disk. Confirmed live via a temporary debug print before
   fixing it. Fix: the six functions now return 1/0 on success/failure;
   db.cpp's wrappers throw database_error on 0, the same idiom
   getActiveServers() already used for read failures.
2. With (1) fixed, db_write_queue's catch block does fire -- but
   src/server.cpp's per-second tick originally reset its failure streak on
   any tick with no *new* failure. Real telemetry only lands every
   position_interval (5s by default), so failures arrive in bursts with
   quiet once-per-second checks in between, and a counter reset by any
   quiet tick could never reach its threshold. Fixed by tracking a
   wall-clock failure *incident* (started at the first new failure, only
   cleared after a 15s quiet recovery window) instead of a per-tick streak;
   once the incident has lasted db_write_failure_disconnect_ticks (default
   5s), every connected client is severed via
   server_clients::disconnectAll() -- matching Tier-3 Path M m05's
   expectation that the server "sever connections cleanly ... rather than
   hanging while accepting traffic it cannot store."

Investigating the real failure mode (a size-capped tmpfs data directory,
see _start_disk_limited_container below) also found that Postgres does not
degrade gracefully once free space drops below roughly one WAL segment
(16MB by default): it PANICs on the WAL write, and because its own crash
*recovery* also needs to write WAL, the whole instance exits rather than
restarting -- this reproduced identically at multiple insert-batch sizes,
so it is a property of Postgres's WAL durability guarantee, not a fixable
test artifact. That matches Tier-3 Path M's own framing of this as a
sever-connections-or-worse event, not a "keeps humming along" one. Given
that, recovery here is demonstrated the way the todo explicitly allows
("either resumes storing or a restart restores service"): a fresh
container + fresh server/client, not resuscitating the dead instance.
"""
from __future__ import annotations

import os
import signal
import socket
import subprocess
import time
import uuid
from pathlib import Path

import psycopg2
import pytest

from conftest import (
    E2E_ROOT,
    FAKE_CLIENT_BIN,
    POSTGIS_IMAGE,
    REPO_ROOT,
    SCHEMA_DIR,
    SERVER_BIN,
    _pick_port,
    _render_template,
    _wait_for,
)


def _start_disk_limited_container(size: str) -> dict[str, object]:
    container = f"fss-e2e-pg-disk-{uuid.uuid4().hex[:8]}"
    password = "e2e"  # noqa: S105 - throwaway password for an ephemeral local test container
    port = _pick_port()
    subprocess.check_call([
        "docker", "run", "--rm", "-d",
        "--name", container,
        "-e", f"POSTGRES_PASSWORD={password}",
        "--tmpfs", f"/var/lib/postgresql:rw,size={size}",
        "-p", f"{port}:5432",
        POSTGIS_IMAGE,
    ], stdout=subprocess.DEVNULL)

    def ready() -> bool:
        try:
            c = psycopg2.connect(
                host="127.0.0.1", port=port, user="postgres", password=password,
                dbname="postgres", connect_timeout=2,
            )
            c.close()
            return True
        except psycopg2.Error:
            return False

    if not _wait_for(ready, 60.0, poll=0.5):
        raise RuntimeError(f"disk-limited postgres container {container} did not become ready in 60s")
    return {
        "host": "127.0.0.1", "port": port, "user": "postgres",
        "password": password, "dbname": "postgres", "container": container,
    }


def _stop_container(container: str) -> None:
    subprocess.run(["docker", "stop", container], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _migrate(conn_params: dict[str, object]) -> None:
    conn = psycopg2.connect(
        host=conn_params["host"], port=conn_params["port"],
        user=conn_params["user"], password=conn_params["password"],
        dbname=conn_params["dbname"],
    )
    conn.autocommit = True
    try:
        with conn.cursor() as cur:
            for sql_file in sorted(SCHEMA_DIR.glob("*.sql")):
                cur.execute(sql_file.read_text())
    finally:
        conn.close()


def _spawn_server(conn_params: dict[str, object], certs_dir: Path, tmp_path: Path, name: str) -> dict[str, object]:
    port = _pick_port()
    config_path = tmp_path / f"server-{name}.json"
    log_path = tmp_path / f"server-{name}.log"
    _render_template(
        E2E_ROOT / "fixtures" / "server.json.tmpl",
        config_path,
        SERVER_PORT=str(port),
        DB_HOST=conn_params["host"],
        DB_PORT=str(conn_params["port"]),
        DB_USER=conn_params["user"],
        DB_PASS=conn_params["password"],
        DB_NAME=conn_params["dbname"],
        CERTS_DIR=str(certs_dir),
    )
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(REPO_ROOT / "src" / ".libs")
    env["PGPORT"] = str(conn_params["port"])
    env["PGPASSWORD"] = str(conn_params["password"])
    log_fp = log_path.open("wb")
    # sourcery skip: dangerous-subprocess-use-audit
    proc = subprocess.Popen(
        [str(SERVER_BIN), str(config_path)],
        cwd=str(REPO_ROOT), env=env, stdout=log_fp, stderr=subprocess.STDOUT,
    )

    def listening() -> bool:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(0.2)
            try:
                s.connect(("127.0.0.1", port))
                return True
            except OSError:
                return False

    if not _wait_for(listening, timeout=10.0):
        proc.terminate()
        raise RuntimeError(f"server-{name} never bound:\n{log_path.read_text(errors='replace')}")
    time.sleep(0.5)
    return {"proc": proc, "port": port, "log": log_path, "log_fp": log_fp, "env": env}


def _stop_proc(proc: subprocess.Popen) -> None:
    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


def _spawn_client(server: dict[str, object], certs_dir: Path, tmp_path: Path, name: str) -> dict[str, object]:
    client_cfg = tmp_path / f"client-{name}.json"
    client_log = tmp_path / f"client-{name}.log"
    _render_template(
        E2E_ROOT / "fixtures" / "client.json.tmpl",
        client_cfg,
        CLIENT_NAME="test1",
        CERTS_DIR=str(certs_dir),
        SERVER_PORT=str(server["port"]),
    )
    log_fp = client_log.open("wb")
    # sourcery skip: dangerous-subprocess-use-audit
    proc = subprocess.Popen(
        [str(FAKE_CLIENT_BIN), str(client_cfg)],
        cwd=str(REPO_ROOT), env=server["env"], stdout=log_fp, stderr=subprocess.STDOUT,
    )
    return {"proc": proc, "log": client_log, "log_fp": log_fp}


@pytest.mark.requires_docker
@pytest.mark.slow
@pytest.mark.timeout(180)
def test_disk_full_fails_visibly_and_a_fresh_instance_recovers(certs_dir, tmp_path):
    """Fill a real, disk-limited Postgres via ongoing INSERT traffic on an
    already-connected server; assert the failure is visible (write-failure
    count logged, clients severed) rather than silently discarded, then
    show a fresh instance (container + server + client) is unaffected --
    "a restart restores service", the todo's explicitly-sanctioned recovery
    path once the underlying Postgres instance itself is gone."""
    db = _start_disk_limited_container("130m")
    server = None
    client = None
    flood_conn = None
    fresh_container = None
    fresh_server = None
    fresh_client = None
    try:
        _migrate(db)
        setup_conn = psycopg2.connect(
            host=db["host"], port=db["port"], user=db["user"], password=db["password"], dbname=db["dbname"],
        )
        setup_conn.autocommit = True
        with setup_conn.cursor() as cur:
            cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
            asset_id = cur.fetchone()[0]

        server = _spawn_server(db, certs_dir, tmp_path, "flood")
        client = _spawn_client(server, certs_dir, tmp_path, "flood")

        def first_row() -> bool:
            with setup_conn.cursor() as cur:
                cur.execute("SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s", (asset_id,))
                return cur.fetchone()[0] > 0

        assert _wait_for(first_row, timeout=25.0), (
            "no baseline position row before flooding the disk\n"
            f"-- server log --\n{server['log'].read_text(errors='replace')}\n"
            f"-- client log --\n{client['log'].read_text(errors='replace')}"
        )

        # Flood via a separate connection while the server's own connection
        # stays open, so its *established* session is the one that
        # experiences the write failure -- not a fresh-connect failure.
        flood_conn = psycopg2.connect(
            host=db["host"], port=db["port"], user=db["user"], password=db["password"], dbname=db["dbname"],
        )
        flood_conn.autocommit = True
        with flood_conn.cursor() as flood_cur:
            for _ in range(2000):
                try:
                    flood_cur.execute(
                        "INSERT INTO assets_assetposition (asset_id, position, altitude) "
                        "SELECT %s, ST_SetSRID(ST_MakePoint(172.6, -43.6), 4326)::geometry, 500 "
                        "FROM generate_series(1, 2000)",
                        (asset_id,),
                    )
                except psycopg2.Error:
                    break

        def failure_logged() -> bool:
            return "DB write failures since start" in server["log"].read_text(errors="replace")

        assert _wait_for(failure_logged, timeout=60.0), (
            "server never logged a DB write failure after the disk filled\n"
            + server["log"].read_text(errors="replace")
        )

        # The failure must escalate to visibly severing the session, not
        # stay a silently-kept-alive one -- give the streak/incident guard
        # its own window (db_write_failure_disconnect_ticks, default 5s,
        # plus margin) *after* the first failure was already observed
        # above, rather than checking instantly. (todo/45+47 unified the
        # severance into the "DB fail-safe tripped ... severed N
        # connection(s)" message.)
        def severed() -> bool:
            return "DB fail-safe tripped" in server["log"].read_text(errors="replace")

        assert _wait_for(severed, timeout=30.0), (
            "server logged write failures but never severed the session\n"
            + server["log"].read_text(errors="replace")
        )

        # Fresh instance: a restart restores service (explicitly sanctioned
        # by the todo as an acceptable recovery path once the flooded
        # instance itself cannot recover in place -- see module docstring).
        fresh_container = _start_disk_limited_container("130m")
        _migrate(fresh_container)
        fresh_setup_conn = psycopg2.connect(
            host=fresh_container["host"], port=fresh_container["port"],
            user=fresh_container["user"], password=fresh_container["password"], dbname=fresh_container["dbname"],
        )
        fresh_setup_conn.autocommit = True
        with fresh_setup_conn.cursor() as cur:
            cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
            fresh_asset_id = cur.fetchone()[0]

        fresh_server = _spawn_server(fresh_container, certs_dir, tmp_path, "fresh")
        fresh_client = _spawn_client(fresh_server, certs_dir, tmp_path, "fresh")

        def fresh_row() -> bool:
            with fresh_setup_conn.cursor() as cur:
                cur.execute(
                    "SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s", (fresh_asset_id,),
                )
                return cur.fetchone()[0] > 0

        assert _wait_for(fresh_row, timeout=25.0), (
            "fresh instance never resumed storing telemetry\n"
            f"-- server log --\n{fresh_server['log'].read_text(errors='replace')}\n"
            f"-- client log --\n{fresh_client['log'].read_text(errors='replace')}"
        )
        fresh_setup_conn.close()
    finally:
        for proc_dict in (client, fresh_client):
            if proc_dict is not None:
                _stop_proc(proc_dict["proc"])
                proc_dict["log_fp"].close()
        for srv_dict in (server, fresh_server):
            if srv_dict is not None:
                _stop_proc(srv_dict["proc"])
                srv_dict["log_fp"].close()
        if flood_conn is not None:
            flood_conn.close()
        if fresh_container is not None:
            _stop_container(str(fresh_container["container"]))
        _stop_container(str(db["container"]))
