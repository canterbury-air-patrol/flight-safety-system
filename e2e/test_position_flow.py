"""Client sends position reports; server persists them to assets_assetposition."""
from __future__ import annotations

import time

import pytest


@pytest.mark.satisfies("TC-SRV-001")
@pytest.mark.requires_docker
def test_position_report_persisted(db_conn, fake_client):
    """fake-client sends position(lat=-43.5, lng=172.5) every 5s; after ~15s
    there should be at least two rows in assets_assetposition with those
    coordinates."""
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]

    fake_client("test1")

    time.sleep(16)

    with db_conn.cursor() as cur:
        cur.execute(
            "SELECT ST_X(position::geometry), ST_Y(position::geometry), altitude "
            "FROM assets_assetposition WHERE asset_id = %s ORDER BY timestamp",
            (asset_id,),
        )
        rows = cur.fetchall()

    assert len(rows) >= 2, f"expected ≥2 position rows, got {len(rows)}"
    for lng, lat, alt in rows:
        assert abs(lng - 172.5) < 1e-6
        assert abs(lat - (-43.5)) < 1e-6
        assert alt == 300


@pytest.mark.satisfies("TC-SRV-001")
@pytest.mark.requires_docker
def test_status_and_search_persisted(db_conn, fake_client):
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]

    fake_client("test1")
    time.sleep(16)

    with db_conn.cursor() as cur:
        cur.execute(
            "SELECT bat_percent, bat_used_mah, bat_volt FROM assets_assetstatus "
            "WHERE asset_id = %s ORDER BY timestamp LIMIT 1",
            (asset_id,),
        )
        status = cur.fetchone()
        cur.execute(
            "SELECT search, search_progress, search_progress_of "
            "FROM assets_assetsearchprogress WHERE asset_id = %s "
            "ORDER BY timestamp LIMIT 1",
            (asset_id,),
        )
        search = cur.fetchone()

    assert status is not None, "no status rows"
    bat_percent, bat_mah, bat_volt = status
    assert bat_percent == 75
    assert bat_mah == 1000
    assert abs(bat_volt - 11.4) < 1e-6

    assert search is not None, "no search rows"
    s_id, progress, total = search
    assert s_id == 1
    assert progress == 23
    assert total == 100
