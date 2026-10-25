"""End-to-end: the 15 s config refresh must not stall command delivery
(todo/91).

The server refreshes every identified client's SMM settings every 15 s, one
SELECT per client on the shared read connection. Run on the command poller,
that held command polling for ~N*L (N clients, L read latency), so a TERM
inserted during the refresh waited out the whole thing.

Read latency is injected by swapping config_assetconfig for a view that sleeps
per statement: the server's SMM read pays it, while the command read (a different
table) does not. A command is inserted every few hundred milliseconds across a
refresh boundary and every dispatch must stay under the latency bound.
"""
from __future__ import annotations

import time
from collections.abc import Iterator
from contextlib import contextmanager

import pytest

from test_command_latency import _wait_for_dispatch

CLIENT_NAMES = ("test1", "test2", "test3")
# Per statement. N * this must exceed LATENCY_BOUND_S so a poller that waits
# out the whole refresh fails, while one that waits out a single read passes.
READ_LATENCY_S = 0.5
LATENCY_BOUND_S = 1.0
IDENTIFY_SETTLE_S = 5.0
# Covers one full 15 s refresh period wherever the test starts within it.
TEST_SPAN_S = 20.0
INSERT_INTERVAL_S = 0.3
DISPATCH_TIMEOUT_S = 6.0


@contextmanager
def slow_asset_config(db_conn, latency_s: float) -> Iterator[None]:
    """Make every statement reading config_assetconfig cost latency_s seconds.

    Per statement, not per row: real latency is a round trip, and a per-row
    sleep would charge a batched read for the rows it saves."""
    with db_conn.cursor() as cur:
        cur.execute("ALTER TABLE config_assetconfig RENAME TO config_assetconfig_real")
        cur.execute(
            "CREATE VIEW config_assetconfig AS "  # noqa: S608 — constants only
            "SELECT r.id, r.asset_id, r.smm_id, r.smm_login, r.smm_password "
            "FROM (SELECT pg_sleep(%s)) s, config_assetconfig_real r",
            (latency_s,),
        )
    try:
        yield
    finally:
        with db_conn.cursor() as cur:
            cur.execute("DROP VIEW config_assetconfig")
            cur.execute("ALTER TABLE config_assetconfig_real RENAME TO config_assetconfig")


# Open until the SMM read is batched (todo/91): the refresh thread's per-client
# SELECTs re-take read_lock back to back and std::mutex is not fair, so the
# command poller still waits out several reads. Drop this marker with the fix.
@pytest.mark.xfail(strict=True, reason="todo/91: per-client SMM reads starve the command poll")
@pytest.mark.requires_docker
@pytest.mark.slow
def test_command_poll_not_stalled_by_config_refresh(db_conn, fake_client, server_proc):
    asset_ids = []
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO config_smmconfig (address, port) VALUES ('127.0.0.1', 8080) RETURNING id")
        smm_id = cur.fetchone()[0]
        for name in CLIENT_NAMES:
            cur.execute("INSERT INTO assets_asset (name) VALUES (%s) RETURNING id", (name,))
            asset_id = cur.fetchone()[0]
            asset_ids.append(asset_id)
            cur.execute(
                "INSERT INTO config_assetconfig (asset_id, smm_id, smm_login, smm_password) "
                "VALUES (%s, %s, 'u', 'p')",
                (asset_id, smm_id),
            )

    for name in CLIENT_NAMES:
        fake_client(name)
    # Identify happens before the latency is injected so only the periodic
    # refresh, not identify-time reads, is being measured.
    time.sleep(IDENTIFY_SETTLE_S)

    latencies = []
    with slow_asset_config(db_conn, READ_LATENCY_S):
        end = time.monotonic() + TEST_SPAN_S
        n = 0
        while time.monotonic() < end:
            asset_id = asset_ids[n % len(asset_ids)]
            n += 1
            with db_conn.cursor() as cur:
                cur.execute(
                    "INSERT INTO assets_assetcommand (asset_id, command, position, altitude) "
                    "VALUES (%s, 'TERM', ST_SetSRID(ST_MakePoint(0, 0), 4326)::geography, 0) "
                    "RETURNING id",
                    (asset_id,),
                )
                dbid = cur.fetchone()[0]
            elapsed = _wait_for_dispatch(server_proc["log"], dbid, DISPATCH_TIMEOUT_S)
            assert elapsed is not None, f"command dbid={dbid} never dispatched during config refresh"
            latencies.append(elapsed)
            time.sleep(INSERT_INTERVAL_S)

    assert latencies
    assert max(latencies) < LATENCY_BOUND_S, (
        f"command dispatch stalled to {max(latencies):.3f}s (bound {LATENCY_BOUND_S}s) "
        f"across the config refresh; {len(latencies)} samples"
    )
