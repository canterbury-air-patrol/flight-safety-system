"""Retired assets are refused, and live sessions are severed (todo/80).

fss-web migration 0013 retires an asset instead of deleting it, so the row and
its audit history survive and the retirement is reversible. The unit suite pins
both halves at their own seams -- db_connection_test.cpp for the queries,
server_clients_test.cpp for the poll/sever split. What only the real stack can
show is that the two halves are actually wired to each other: that setting one
column in the database, with nothing else touched and no restart, takes a flying
aircraft off the server and keeps it off until the column is cleared again.

The bound under test is the documented one from
docs/decisions/80-retired-asset-enforcement.md: the poller observes retirement
within one second and the main loop severs on its next 100 ms tick.
"""
from __future__ import annotations

import time

import pytest

# Worst-case observed-to-severed is ~1.1 s by design. The margin is for process
# scheduling and the log write, not for a slower enforcement path -- a failure
# here means the bound moved, which is a documentation change as much as a code
# one.
SEVER_TIMEOUT_S = 10.0


def _server_log_text(server_proc) -> str:
    return server_proc["log"].read_text(errors="replace")


def _wait_for_text(server_proc, needle: str, timeout: float = 15.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if needle in _server_log_text(server_proc):
            return True
        time.sleep(0.05)
    return False


def _identify_count(server_proc, name: str) -> int:
    return _server_log_text(server_proc).count(f"Aircraft client identified: {name}")


def _wait_for_identify_count(server_proc, name: str, count: int, timeout: float = 15.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if _identify_count(server_proc, name) >= count:
            return True
        time.sleep(0.05)
    return False


def _position_count(db_conn, asset_id: int) -> int:
    with db_conn.cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s", (asset_id,)
        )
        return cur.fetchone()[0]


@pytest.mark.satisfies("TC-SRV-008")
@pytest.mark.requires_docker
def test_retirement_severs_a_live_session_and_reactivation_restores_it(
    db_conn, reset_db, fake_client, server_proc
):
    """The whole reversible lifecycle against a real server and database.

    Deliberately one test rather than three: reactivation is only meaningful as
    the state *after* a severance, and the id-preservation assertion at the end
    is the one that would catch retirement being implemented as a delete. Split
    up, each part would need to rebuild the previous part's state anyway.
    """
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]

    # --- active: identifies and flies normally -----------------------------
    fake_client("test1")
    assert _wait_for_identify_count(server_proc, "test1", 1), (
        "client never identified while its asset was active\n"
        + _server_log_text(server_proc)
    )

    # Telemetry is landing, so the severance below is removing something real.
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline and _position_count(db_conn, asset_id) < 1:
        time.sleep(0.1)
    assert _position_count(db_conn, asset_id) >= 1, (
        "no position rows stored while the asset was active; the rest of this "
        "test cannot distinguish enforcement from a client that was never flying"
    )

    # --- retire: the live session is severed within the documented bound ----
    with db_conn.cursor() as cur:
        cur.execute(
            "UPDATE assets_asset SET retired_at = NOW() WHERE id = %s", (asset_id,)
        )

    assert _wait_for_text(
        server_proc,
        f"Disconnecting session for retired asset_id {asset_id}",
        timeout=SEVER_TIMEOUT_S,
    ), (
        f"server did not sever the retired asset's session within "
        f"{SEVER_TIMEOUT_S}s\n" + _server_log_text(server_proc)
    )

    # Nothing more is accepted for that session. Sampled after a pause rather
    # than immediately: writes already queued when the severance happened may
    # still drain under the existing writer contract, and that is allowed --
    # what must not happen is the count continuing to climb afterwards.
    time.sleep(3)
    settled = _position_count(db_conn, asset_id)
    time.sleep(3)
    assert _position_count(db_conn, asset_id) == settled, (
        "position rows kept arriving after the session was severed for "
        "retirement; the aircraft is still being accepted"
    )

    # --- retired: its own reconnects are refused at identify ----------------
    # No second fake_client is launched: the severed client redials on its own
    # (examples/fake_client.cpp retries once a second), which is exactly the
    # real behaviour under test -- an aircraft does not accept being cut off,
    # so "severed" is only meaningful if the reconnect is refused too.
    assert _wait_for_text(
        server_proc,
        "Rejecting identity 'test1': no matching asset in the database",
    ), (
        "a retired asset's reconnect was not refused at identify\n"
        + _server_log_text(server_proc)
    )
    assert _identify_count(server_proc, "test1") == 1, (
        "a retired asset identified again on reconnect\n" + _server_log_text(server_proc)
    )

    # --- reactivate: back in service under the original id ------------------
    with db_conn.cursor() as cur:
        cur.execute(
            "UPDATE assets_asset SET retired_at = NULL WHERE id = %s", (asset_id,)
        )

    # Same client, still redialing -- nothing is restarted and no new process
    # is introduced, so a success here is the reactivation and nothing else.
    assert _wait_for_identify_count(server_proc, "test1", 2, timeout=SEVER_TIMEOUT_S), (
        "reactivated asset never identified again -- if the asset_owners claim "
        "from the severed session was not released, the id stays unclaimable "
        "until the server restarts\n" + _server_log_text(server_proc)
    )

    # The row was never deleted, so the id and the history hanging off it are
    # the ones the asset always had. This is what retirement buys over a delete.
    with db_conn.cursor() as cur:
        cur.execute("SELECT id FROM assets_asset WHERE name = 'test1'")
        assert cur.fetchone()[0] == asset_id
    assert _position_count(db_conn, asset_id) >= settled, (
        "position history from before retirement was lost across the cycle"
    )

    # And it is flying again: new rows land under the original id.
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline and _position_count(db_conn, asset_id) <= settled:
        time.sleep(0.1)
    assert _position_count(db_conn, asset_id) > settled, (
        "reactivated asset identified but its telemetry is not being stored"
    )
