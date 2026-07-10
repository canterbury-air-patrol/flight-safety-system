"""Two clients presenting the same asset identity (todo/31, TC-MAV-015 server
share): decided policy is a configurable server setting, defaulting to
reject-newcomer. Keep the existing session; refuse the new connection at
identify. Commands must route to exactly one session, never two."""
from __future__ import annotations

import time

import pytest


def _server_log_text(server_proc) -> str:
    return server_proc["log"].read_text(errors="replace")


def _wait_for_identify_count(server_proc, name: str, count: int, timeout: float = 15.0) -> bool:
    """Block until "Aircraft client identified: <name>" appears `count` times
    in the server log, or time out."""
    needle = f"Aircraft client identified: {name}"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if _server_log_text(server_proc).count(needle) >= count:
            return True
        time.sleep(0.05)
    return False


def _wait_for_text(server_proc, needle: str, timeout: float = 15.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if needle in _server_log_text(server_proc):
            return True
        time.sleep(0.05)
    return False


@pytest.mark.requires_docker
def test_duplicate_identity_rejects_newcomer_by_default(db_conn, fake_client, server_proc):
    """Default policy (duplicate_identity_reject_newcomer): the first session
    stays up; a second connection presenting the same identity is rejected
    at identify and never receives commands."""
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]

    # Same identity/cert (name="test1") for both, but distinct client_id so
    # each gets its own config/log file path.
    first = fake_client("test1", client_id="test1-first")
    assert _wait_for_identify_count(server_proc, "test1", 1), (
        "first client never identified\n" + _server_log_text(server_proc)
    )

    second = fake_client("test1", client_id="test1-second")
    assert _wait_for_text(server_proc, "Rejecting duplicate identity for asset_id"), (
        "server never logged a duplicate-identity rejection for the second client\n"
        + _server_log_text(server_proc)
    )

    # Only ever one successful identify -- the newcomer never got in.
    assert _server_log_text(server_proc).count("Aircraft client identified: test1") == 1, (
        "expected exactly one successful identify\n" + _server_log_text(server_proc)
    )
    # The first session was not disturbed by the rejected newcomer.
    assert first["proc"].poll() is None, (
        "first client process should stay alive across a rejected duplicate\n"
        + first["log"].read_text(errors="replace")
    )

    # A command must route to the (sole) live session, never the rejected one.
    with db_conn.cursor() as cur:
        cur.execute(
            "INSERT INTO assets_assetcommand (asset_id, command, position, altitude) "
            "VALUES (%s, 'RTL', ST_SetSRID(ST_MakePoint(0, 0), 4326)::geography, 0)",
            (asset_id,),
        )
    time.sleep(6)  # command-poller cadence

    first_log = first["log"].read_text(errors="replace")
    assert "RCVD_CMD: RTL" in first_log, (
        "command was not delivered to the surviving session\n" + first_log
    )
    second_log = second["log"].read_text(errors="replace")
    assert "RCVD_CMD: RTL" not in second_log, (
        "command must never reach the rejected duplicate session\n" + second_log
    )

    # No telemetry interleaving: every stored position row for this asset
    # must have landed while only one session was ever identified -- already
    # established above (exactly one successful identify total), so a
    # non-zero row count here is unambiguously the surviving session's.
    with db_conn.cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s", (asset_id,),
        )
        pos_count = cur.fetchone()[0]
    assert pos_count >= 1, "surviving session should still be reporting position"
