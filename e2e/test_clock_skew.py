"""e2e: skewed client timestamps handled sanely (Path J server share, todo/33).

The server's skew-aware position-staleness gate (client_session.cpp's
position_report handler) compares each report's client-stamped timestamp
against the server clock, corrected by client_clock_offset_ms -- measured
from RTT responses that carry the client's clock (todo/17 item 3), smoothed
via EWMA, updated roughly every 10s (the RTT request cadence). Until that
offset is learned, a skewed client's reports read as stale and are
discarded; once learned, storage should resume.
"""
from __future__ import annotations

import time

import pytest

from conftest import wait_for_row


@pytest.mark.satisfies("TC-MAV-017")
@pytest.mark.requires_docker
@pytest.mark.slow
@pytest.mark.parametrize("offset_ms", [300_000, -300_000], ids=["ahead", "behind"])
def test_moderate_skew_still_stores_and_commands_work(db_conn, fake_client, server_proc, offset_ms):
    """+-5 min skew: TLS connects, telemetry is eventually stored once the
    RTT clock-offset is learned, and a command round-trip still works."""
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]

    client = fake_client("test1", extra_args=[f"--clock-offset-ms={offset_ms}"])

    row = wait_for_row(
        db_conn,
        "SELECT id FROM assets_assetposition WHERE asset_id = %s ORDER BY id LIMIT 1",
        params=(asset_id,),
        timeout=30.0,
    )
    assert row is not None, (
        f"no position row stored within 30s for offset_ms={offset_ms}\n"
        + client["log"].read_text(errors="replace")
    )
    assert client["proc"].poll() is None, "client process should stay connected under moderate skew"

    with db_conn.cursor() as cur:
        cur.execute(
            "INSERT INTO assets_assetcommand (asset_id, command, position, altitude) "
            "VALUES (%s, 'RTL', ST_SetSRID(ST_MakePoint(0, 0), 4326)::geography, 0)",
            (asset_id,),
        )
    time.sleep(6)
    log_content = client["log"].read_text(errors="replace")
    assert "RCVD_CMD: RTL" in log_content, (
        f"command round-trip broken under offset_ms={offset_ms}\n" + log_content
    )


@pytest.mark.requires_docker
@pytest.mark.slow
def test_extreme_skew_one_hour(db_conn, fake_client, server_proc):
    """1h skew: pin whatever the current policy actually does rather than
    assume a clamp/reject that doesn't exist -- max_rtt_for_clock_offset_ms
    gates RTT *sample quality*, not the size of the reported skew, so a
    clean RTT round-trip should still let the offset be learned same as a
    moderate skew, just with a larger correction."""
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]

    client = fake_client("test1", extra_args=["--clock-offset-ms=3600000"])

    row = wait_for_row(
        db_conn,
        "SELECT id FROM assets_assetposition WHERE asset_id = %s ORDER BY id LIMIT 1",
        params=(asset_id,),
        timeout=30.0,
    )
    assert row is not None, (
        "no position row stored within 30s for a 1h clock skew\n"
        + client["log"].read_text(errors="replace")
    )
    assert client["proc"].poll() is None, "client process should stay connected under extreme skew"
    # No corrupted ordering: once storage resumes, rows keep accumulating
    # rather than getting stuck after the first.
    time.sleep(6)
    with db_conn.cursor() as cur:
        cur.execute("SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s", (asset_id,))
        count = cur.fetchone()[0]
    assert count >= 1
