"""End-to-end: a black-holed database must not stall the main loop (todo/46).

The server's reconnect health check (db_ping, a live SELECT 1) used to run on
the main loop. Against a *frozen* DB host the ping's query is ACKed by the
peer's kernel but never answered, so nothing client-side — keepalives or
tcp_user_timeout — can unwedge it: the call blocks until the host thaws. On
the pre-fix binary that froze command dispatch, timeout monitoring, RTT
scheduling and the DB fail-safe tick for the duration of the freeze.

Injection is `docker pause` on the Postgres container, which is exactly that
frozen-host black-hole (the container's kernel netns stays up and ACKs
everything; no RST, no retransmission) and is trivially reversible with no
container-IP churn. It complements test_db_disk_full.py (clean, fast-failing
DB errors) and test_slow_db_does_not_stall.py (a politely blocking lock).

Liveness is observed client-side: fake_client logs RCVD_RTT_REQ for each RTT
request, and RTT requests are sent by the same per-second main-loop tick that
drives command dispatch and the fail-safe. A healthy client sees ~1 request
per second: the tick sends a fresh request whenever none is outstanding, and
rtt_retry_interval (src/client_session.cpp) only throttles re-requests while
one is unanswered. On the pre-fix binary the tick wedges inside db_ping
within 1s of the freeze and the flow stops entirely (verified by probing the
pre-fix binary: 0 new requests over a 60s freeze, after the single request
enqueued before the wedge landed). The counting window therefore starts
*after* the pause takes effect plus a settle: sends already in flight when
the freezer lands must not leak into the count — with a ~1/s flow that leak
alone once masked a wedge.

The fail-safe must NOT trip: a frozen DB makes writes hang, not fail, so
write_failure_count() never moves — pinning that a stall is not misclassified
as the todo/45+47 write-failure outage.
"""
from __future__ import annotations

import subprocess
import time
from pathlib import Path

import pytest

from conftest import wait_for_row

# A live main loop yields ~1 RTT request/s (see module docstring), so a 25s
# freeze should show ~24; require 5 as a generous scheduling margin. A wedged
# loop shows 0 — the settle after pausing excludes in-flight sends. 25s also
# spans a full 15s server-list broadcast period and several fail-safe ticks.
PAUSE_S = 25
PAUSE_SETTLE_S = 2
MIN_RTT_REQS_DURING_PAUSE = 5


def _read_from(path: Path, offset: int) -> str:
    with path.open("rb") as fp:
        fp.seek(offset)
        return fp.read().decode(errors="replace")


def _wait_for_log(path: Path, needle: str, timeout: float, offset: int = 0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if needle in _read_from(path, offset):
            return True
        time.sleep(0.2)
    return False


@pytest.mark.requires_docker
@pytest.mark.slow
@pytest.mark.timeout(180)
def test_frozen_db_does_not_stall_main_loop(
    db_conn, fake_client, migrated_db, server_proc
):
    container = migrated_db["container"]
    if container is None:
        pytest.skip("needs a docker-managed postgres (external DB can't be paused)")

    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]

    client = fake_client("test1")
    client_log: Path = client["log"]
    server_log: Path = server_proc["log"]

    # Baseline: telemetry lands and the RTT tick is observably running.
    assert wait_for_row(
        db_conn,
        "SELECT id FROM assets_assetposition WHERE asset_id = %s LIMIT 1",
        params=(asset_id,),
        timeout=15.0,
    ) is not None, "no baseline position row before the freeze"
    assert _wait_for_log(client_log, "RCVD_RTT_REQ", timeout=30.0), (
        "no baseline RTT request before the freeze"
    )

    # Freeze the DB. From here until unpause, this test must not touch
    # db_conn either — its queries would hang exactly like the server's.
    # The counting window opens only after the freeze has landed and
    # in-flight sends have settled (see module docstring).
    paused = False
    try:
        subprocess.run(["docker", "pause", container], check=True)
        paused = True
        time.sleep(PAUSE_SETTLE_S)
        pause_offset = client_log.stat().st_size
        time.sleep(PAUSE_S)
        frozen_window = _read_from(client_log, pause_offset)
    finally:
        if paused:
            subprocess.run(["docker", "unpause", container], check=True)

    # The main loop kept ticking while both DB connections were wedged: RTT
    # requests arrived on cadence. Pre-fix this is 0 — the per-second tick
    # blocks inside db_ping within 1s of the freeze.
    rtt_reqs = frozen_window.count("RCVD_RTT_REQ")
    assert rtt_reqs >= MIN_RTT_REQS_DURING_PAUSE, (
        f"only {rtt_reqs} RTT request(s) reached the client during a "
        f"{PAUSE_S}s DB freeze — the main loop stalled"
    )
    assert server_proc["proc"].poll() is None, "server died during the DB freeze"

    # A frozen DB hangs writes rather than failing them, so the todo/45+47
    # write-failure fail-safe must not have fired.
    assert "DB fail-safe tripped" not in server_log.read_text(errors="replace"), (
        "a DB stall was misclassified as a write-failure outage"
    )

    # Recovery: the hung statements complete, the write queue drains, and
    # fresh telemetry keeps landing.
    with db_conn.cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s",
            (asset_id,),
        )
        rows_after_unpause = cur.fetchone()[0]
    assert wait_for_row(
        db_conn,
        "SELECT 1 FROM assets_assetposition WHERE asset_id = %s "
        "HAVING COUNT(*) > %s",
        params=(asset_id, rows_after_unpause),
        timeout=20.0,
    ) is not None, "telemetry did not resume after the DB thawed"

    # And the command path recovered end to end: a TERM inserted now reaches
    # the aircraft.
    command_offset = client_log.stat().st_size
    with db_conn.cursor() as cur:
        cur.execute(
            "INSERT INTO assets_assetcommand (asset_id, command, position, altitude) "
            "VALUES (%s, 'TERM', "
            "        ST_SetSRID(ST_MakePoint(0, 0), 4326)::geography, 0)",
            (asset_id,),
        )
    assert _wait_for_log(client_log, "RCVD_CMD", timeout=10.0, offset=command_offset), (
        "TERM did not reach the client after the DB thawed"
    )
