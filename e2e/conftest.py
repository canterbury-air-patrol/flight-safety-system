"""Shared pytest fixtures for the E2E harness.

Layout:
    pg_container   — docker-run a postgis database, yield a connection dict
    migrated_db    — apply e2e/schema/*.sql in order
    certs_dir      — per-session cert tree built via certs/generate-*.sh
    db_conn        — psycopg2 connection bound to migrated_db
    server_proc    — spawn src/fss-server against migrated_db + certs_dir
    fake_client    — factory that spawns examples/fss-fake-client
"""
from __future__ import annotations

import os
import shutil
import signal
import socket
import string
import subprocess
import time
import uuid
from collections.abc import Callable, Iterator
from pathlib import Path

import psycopg2
import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
E2E_ROOT = Path(__file__).resolve().parent
SERVER_BIN = REPO_ROOT / "src" / "fss-server"
FAKE_CLIENT_BIN = REPO_ROOT / "examples" / "fss-fake-client"
CERT_SCRIPTS = REPO_ROOT / "certs"
SCHEMA_DIR = E2E_ROOT / "schema"

POSTGIS_IMAGE = "postgis/postgis:18-3.6-alpine"
STARTUP_TIMEOUT_S = 60


def _pick_port() -> int:
    """Ask the kernel for an unused TCP port, return it, close the socket."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _wait_for(predicate: Callable[[], bool], timeout: float, poll: float = 0.1) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(poll)
    return False


def _docker_available() -> bool:
    if not shutil.which("docker"):
        return False
    r = subprocess.run(
        ["docker", "info"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    return r.returncode == 0


@pytest.fixture(scope="session")
def pg_container() -> Iterator[dict[str, object]]:
    """Provide Postgres+PostGIS connection parameters.

    Three modes (checked in order):
    1. FSS_E2E_EXTERNAL_DB is set — use an already-running Postgres (e.g.
       a GitHub Actions service container).  No Docker is started.
    2. FSS_E2E_DOCKER_HOST_NET=1 — start postgis/postgis with --network=host
       so the container shares the host network (needed on some sandboxes).
    3. Default — start postgis/postgis with a mapped port.
    """
    external = os.environ.get("FSS_E2E_EXTERNAL_DB")
    if external:
        # Parse simple "key=value ..." style connection string.
        params: dict[str, object] = {}
        for token in external.split():
            k, _, v = token.partition("=")
            params[k] = v
        params.setdefault("host", "127.0.0.1")
        params.setdefault("port", 5432)
        params.setdefault("user", "postgres")
        params.setdefault("password", "e2e")
        params.setdefault("dbname", "postgres")
        params["container"] = None
        params["port"] = int(params["port"])

        def ready() -> bool:
            try:
                c = psycopg2.connect(
                    host=params["host"], port=params["port"],
                    user=params["user"], password=params["password"],
                    dbname=params["dbname"], connect_timeout=2,
                )
                c.close()
                return True
            except psycopg2.Error:
                return False

        if not _wait_for(ready, STARTUP_TIMEOUT_S, poll=0.5):
            raise RuntimeError("external postgres did not become ready")
        yield params
        return

    if not _docker_available():
        pytest.skip("docker daemon not reachable")

    container = f"fss-e2e-pg-{uuid.uuid4().hex[:8]}"
    password = "e2e"
    user = "postgres"
    db = "postgres"

    # On sandboxes where Docker's default bridge network is blocked,
    # set FSS_E2E_DOCKER_HOST_NET=1 to share the host network namespace.
    if os.environ.get("FSS_E2E_DOCKER_HOST_NET") == "1":
        port = _pick_port()
        net_args = ["--network=host", "-e", f"PGPORT={port}"]
    else:
        port = _pick_port()
        net_args = ["-p", f"{port}:5432"]

    subprocess.check_call([
        "docker", "run", "--rm", "-d",
        "--name", container,
        "-e", f"POSTGRES_PASSWORD={password}",
        *net_args,
        POSTGIS_IMAGE,
    ], stdout=subprocess.DEVNULL)

    def ready() -> bool:
        try:
            conn = psycopg2.connect(
                host="127.0.0.1", port=port,
                user=user, password=password, dbname=db,
                connect_timeout=2,
            )
            conn.close()
            return True
        except psycopg2.Error:
            return False

    try:
        if not _wait_for(ready, STARTUP_TIMEOUT_S, poll=0.5):
            logs = subprocess.run(
                ["docker", "logs", container], capture_output=True, text=True
            )
            raise RuntimeError(
                f"postgres container did not become ready in {STARTUP_TIMEOUT_S}s\n"
                f"{logs.stdout}\n{logs.stderr}"
            )
        yield {
            "host": "127.0.0.1",
            "port": port,
            "user": user,
            "password": password,
            "dbname": db,
            "container": container,
        }
    finally:
        subprocess.run(["docker", "stop", container],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


@pytest.fixture(scope="session")
def migrated_db(pg_container: dict[str, object]) -> dict[str, object]:
    """Apply bundled SQL files in lexical order and yield the same conn info.

    This uses the fallback schema under e2e/schema/. The primary path
    (running the real fss-web Django migrations) is not implemented here
    because fss-web is not part of this repo.
    """
    conn = psycopg2.connect(
        host=pg_container["host"], port=pg_container["port"],
        user=pg_container["user"], password=pg_container["password"],
        dbname=pg_container["dbname"],
    )
    conn.autocommit = True
    try:
        with conn.cursor() as cur:
            for sql_file in sorted(SCHEMA_DIR.glob("*.sql")):
                cur.execute(sql_file.read_text())
    finally:
        conn.close()
    return pg_container


@pytest.fixture
def db_conn(migrated_db: dict[str, object]) -> Iterator[psycopg2.extensions.connection]:
    conn = psycopg2.connect(
        host=migrated_db["host"], port=migrated_db["port"],
        user=migrated_db["user"], password=migrated_db["password"],
        dbname=migrated_db["dbname"],
    )
    conn.autocommit = True
    try:
        yield conn
    finally:
        conn.close()


@pytest.fixture
def reset_db(db_conn: psycopg2.extensions.connection) -> Iterator[None]:
    """Truncate FSS tables before the test so state doesn't bleed between cases."""
    tables = [
        "assets_assetposition",
        "assets_assetrtt",
        "assets_assetstatus",
        "assets_assetsearchprogress",
        "assets_assetcommand",
        "config_assetconfig",
        "config_smmconfig",
        "config_serverconfig",
        "assets_asset",
    ]
    with db_conn.cursor() as cur:
        # sourcery skip: sqlalchemy-execute-raw-query
        cur.execute("TRUNCATE " + ", ".join(tables) + " RESTART IDENTITY CASCADE")
    yield


@pytest.fixture(scope="session")
def certs_dir(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Generate CA + localhost server cert + a pool of client certs.

    We reuse the project's generate-*.sh scripts so the cert layout matches
    what the server/client binaries expect. Generated once per test session.
    """
    if not CERT_SCRIPTS.exists():
        pytest.skip(f"cert scripts not found at {CERT_SCRIPTS}")

    target = tmp_path_factory.mktemp("certs")
    for name in ("generate-ca.sh", "generate-server.sh", "generate-client.sh",
                 "ca.tmpl", "client.tmpl", "localhost.tmpl"):
        src = CERT_SCRIPTS / name
        if src.exists():
            shutil.copy(src, target / name)
    for script in ("generate-ca.sh", "generate-server.sh", "generate-client.sh"):
        (target / script).chmod(0o755)

    # sourcery skip: dangerous-subprocess-use-audit
    def run(cmd: list[str]) -> None:
        subprocess.check_call(cmd, cwd=str(target))

    run(["./generate-ca.sh"])
    run(["./generate-server.sh", "localhost"])
    for client_name in ("test1", "test2", "test3"):
        run(["./generate-client.sh", client_name])
    return target


def _render_template(template_path: Path, out_path: Path, **subs: object) -> None:
    tmpl = string.Template(template_path.read_text())
    out_path.write_text(tmpl.substitute(**subs))


@pytest.fixture
def server_proc(
    migrated_db: dict[str, object],
    certs_dir: Path,
    tmp_path: Path,
    reset_db: None,
) -> Iterator[dict[str, object]]:
    """Spawn fss-server pointed at migrated_db. Yield a control dict.

    The test is responsible for asserting behavior; this fixture only handles
    config rendering, startup readiness (listening TCP socket) and teardown.
    """
    if not SERVER_BIN.exists():
        pytest.skip(f"fss-server not built at {SERVER_BIN}")

    port = _pick_port()
    config_path = tmp_path / "server.json"
    log_path = tmp_path / "server.log"
    _render_template(
        E2E_ROOT / "fixtures" / "server.json.tmpl",
        config_path,
        SERVER_PORT=str(port),
        DB_HOST=migrated_db["host"],
        DB_PORT=str(migrated_db["port"]),
        DB_USER=migrated_db["user"],
        DB_PASS=migrated_db["password"],
        DB_NAME=migrated_db["dbname"],
        CERTS_DIR=str(certs_dir),
    )

    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(REPO_ROOT / "src" / ".libs")
    env["PGPORT"] = str(migrated_db["port"])
    env["PGPASSWORD"] = str(migrated_db["password"])

    log_fp = log_path.open("wb")
    # sourcery skip: dangerous-subprocess-use-audit
    proc = subprocess.Popen(
        [str(SERVER_BIN), str(config_path)],
        cwd=str(REPO_ROOT),
        env=env,
        stdout=log_fp,
        stderr=subprocess.STDOUT,
    )

    def listening() -> bool:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(0.2)
            try:
                s.connect(("127.0.0.1", port))
                return True
            except OSError:
                return False

    try:
        if not _wait_for(listening, timeout=10, poll=0.1):
            proc.terminate()
            proc.wait(timeout=5)
            log_fp.close()
            raise RuntimeError(
                f"fss-server did not begin listening on port {port} in 10s:\n"
                f"{log_path.read_text()}"
            )
        # The readiness probe opens a raw TCP connection that triggers a
        # failed TLS handshake on the server's accept thread.  Give the
        # thread a moment to recover before yielding control to the test.
        time.sleep(0.5)
        yield {
            "proc": proc,
            "port": port,
            "config": config_path,
            "log": log_path,
        }
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        log_fp.close()


@pytest.fixture
def fake_client(
    server_proc: dict[str, object],
    certs_dir: Path,
    tmp_path: Path,
) -> Iterator[Callable[..., dict[str, object]]]:
    """Factory that spawns an fss-fake-client bound to the active server."""
    if not FAKE_CLIENT_BIN.exists():
        pytest.skip(f"fss-fake-client not built at {FAKE_CLIENT_BIN}")

    procs: list[dict[str, object]] = []

    def _launch(
        name: str = "test1",
        ca_override: Path | None = None,
    ) -> dict[str, object]:
        config_path = tmp_path / f"client-{name}.json"
        log_path = tmp_path / f"client-{name}.log"
        _render_template(
            E2E_ROOT / "fixtures" / "client.json.tmpl",
            config_path,
            CLIENT_NAME=name,
            CERTS_DIR=str(ca_override if ca_override else certs_dir),
            SERVER_PORT=str(server_proc["port"]),
        )
        env = os.environ.copy()
        env["LD_LIBRARY_PATH"] = str(REPO_ROOT / "src" / ".libs")
        log_fp = log_path.open("wb")
        # sourcery skip: dangerous-subprocess-use-audit
        proc = subprocess.Popen(
            [str(FAKE_CLIENT_BIN), str(config_path)],
            cwd=str(REPO_ROOT),
            env=env,
            stdout=log_fp,
            stderr=subprocess.STDOUT,
        )
        handle = {"proc": proc, "log": log_path, "config": config_path, "log_fp": log_fp}
        procs.append(handle)
        return handle

    try:
        yield _launch
    finally:
        for handle in procs:
            proc = handle["proc"]
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
            handle["log_fp"].close()


def wait_for_row(
    conn: psycopg2.extensions.connection,
    query: str,
    params: tuple = (),
    timeout: float = 20.0,
    poll: float = 0.5,
) -> tuple | None:
    """Poll a query until it returns a row or timeout elapses."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        with conn.cursor() as cur:
            cur.execute(query, params)
            row = cur.fetchone()
            if row is not None:
                return row
        time.sleep(poll)
    return None
