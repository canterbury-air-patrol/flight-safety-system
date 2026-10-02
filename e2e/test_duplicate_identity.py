"""Two clients presenting the same asset identity (todo/31, TC-MAV-015 server
share): decided policy is a configurable server setting, defaulting to
reject-newcomer. Keep the existing session; refuse the new connection at
identify. Commands must route to exactly one session, never two."""
from __future__ import annotations

import time
import uuid

import pytest

from conftest import wait_for_row


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


@pytest.mark.satisfies("TC-MAV-015")
@pytest.mark.requires_docker
@pytest.mark.parametrize("server_proc", [{}, {"duplicate_identity_policy": "evict_oldest"}], indirect=True)
def test_duplicate_identity_records_both_sessions(db_conn, fake_client, server_proc, request):
    """Cloned credentials still produce distinguishable session evidence."""
    policy = request.node.callspec.params["server_proc"].get("duplicate_identity_policy", "reject_newcomer")
    evict = policy == "evict_oldest"
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]
    first = fake_client("test1", client_id="incumbent")
    assert _wait_for_identify_count(server_proc, "test1", 1)
    second = fake_client("test1", client_id="newcomer")
    row = wait_for_row(
        db_conn,
        "SELECT event_id, outcome, incumbent, newcomer, timestamp <= received_at, "
        "acknowledged_at, acknowledged_by_id, acknowledged_username "
        "FROM assets_assetidentityevent WHERE asset_id = %s ORDER BY id LIMIT 1",
        (asset_id,),
    )
    assert row is not None
    event_id, outcome, incumbent, newcomer, timely, ack_at, ack_by, ack_name = row
    assert uuid.UUID(str(event_id)).version == 4
    assert outcome == ("incumbent_evicted" if evict else "newcomer_rejected")
    assert timely
    assert (ack_at, ack_by, ack_name) == (None, None, "")
    assert incumbent["session_id"] != newcomer["session_id"]
    assert incumbent["certificate_cn"] == newcomer["certificate_cn"] == "test1"
    assert incumbent["certificate_sha256"] == newcomer["certificate_sha256"]
    assert len(incumbent["certificate_sha256"]) == 64
    for peer in (incumbent, newcomer):
        assert peer["peer_address"]
        assert all(len(value) <= 512 for value in peer.values())
    assert _wait_for_identify_count(server_proc, "test1", 2 if evict else 1)
    with db_conn.cursor() as cur:
        cur.execute(
            "INSERT INTO assets_assetcommand (asset_id, command) VALUES (%s, 'RTL')", (asset_id,),
        )
    winner, loser = (second, first) if evict else (first, second)
    assert _wait_for_text(winner, "RCVD_CMD: RTL")
    assert "RCVD_CMD: RTL" not in loser["log"].read_text(errors="replace")
