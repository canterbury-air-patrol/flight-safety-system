"""Server sends RTT requests every 10s and records replies into assets_assetrtt."""
from __future__ import annotations

import pytest

from conftest import wait_for_row


@pytest.mark.requires_docker
@pytest.mark.slow
def test_rtt_round_trip_recorded(db_conn, fake_client):
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]

    fake_client("test1")

    # RTT cadence is 10s (commit c9cbd18); give a generous window for the
    # first request + reply + DB insert.
    row = wait_for_row(
        db_conn,
        "SELECT rtt FROM assets_assetrtt WHERE asset_id = %s ORDER BY timestamp LIMIT 1",
        params=(asset_id,),
        timeout=30.0,
    )
    assert row is not None, "no rtt row appeared within 30s"
    (rtt_ms,) = row
    assert 0 <= rtt_ms < 5000, f"rtt_ms out of range: {rtt_ms}"
