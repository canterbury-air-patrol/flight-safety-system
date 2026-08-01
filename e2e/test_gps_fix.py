"""A client that loses its GPS fix is recorded as such, not silently dropped.

Before todo/76 a no-fix report was discarded, so the operator saw a position
that stopped advancing while RTT kept the asset connected -- indistinguishable
from a dropout or a wedged FMU. The rows now carry gps_fix_valid, and a report
with no coordinates at all stores NULL geometry rather than nothing.
"""
from __future__ import annotations

import time

import pytest


@pytest.mark.satisfies("TC-FS-002")
@pytest.mark.requires_docker
def test_no_fix_reports_are_recorded_with_null_position(db_conn, fake_client):
    """Drive good -> no fix -> good and assert the transition is visible.

    The fake client clears the coords-valid flag and NaNs the coordinates
    together, exactly as cap-fmu does, so this exercises the whole signal.
    """
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]

    fake_client(
        "test1",
        extra_args=[
            "--position-interval-ms=1000",
            "--no-fix-start-ms=6000",
            "--no-fix-stop-ms=12000",
        ],
    )

    time.sleep(18)

    with db_conn.cursor() as cur:
        cur.execute(
            "SELECT gps_fix_valid, position IS NULL FROM assets_assetposition "
            "WHERE asset_id = %s ORDER BY timestamp",
            (asset_id,),
        )
        rows = cur.fetchall()

    assert len(rows) >= 6, f"expected several position rows, got {len(rows)}"

    fix_states = [valid for valid, _ in rows]
    assert True in fix_states, "no GPS-backed rows were recorded"
    assert False in fix_states, "the no-fix window left no record at all"

    # Every no-fix row here has NULL geometry: the client sent the sentinel
    # rather than a dead-reckoned estimate, and NULL must not decay to (0,0) --
    # Null Island is a legal coordinate that would render as a real position.
    for valid, position_is_null in rows:
        assert position_is_null == (not valid), (
            f"gps_fix_valid={valid} paired with position_is_null={position_is_null}"
        )

    # The outage is bounded: the fix comes back, and the run of no-fix rows sits
    # between GPS-backed ones rather than running to the end of the session.
    assert fix_states[0] is True, "session did not start with a GPS-backed row"
    assert fix_states[-1] is True, "fix never recovered"
