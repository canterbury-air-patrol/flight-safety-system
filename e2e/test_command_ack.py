"""End-to-end check of the command-acknowledgement storage leg (todo/17 item 1).

When the command-ack capability is negotiated, the fake client replies to a
dispatched command with the two-phase ack (received, then actioned). This test
drives a command through the real server and asserts the server:
  - records the dispatched message id on the command row (dispatch_id), and
  - stores the ack against that row (ack_state advances to actioned,
    ack_timestamp populated).

This exercises the server routing + DB write that unit tests cover only against
a mock — here it runs through the live transport, processMessage, and the async
write queue into the database."""
from __future__ import annotations

import time

import pytest


# Mirror flight_safety_system::transport::fss_command_ack_outcome.
ACK_STATE_RECEIVED = 0
ACK_STATE_ACTIONED = 1


def _poll_row(db_conn, dbid: int, timeout: float):
    """Poll the AssetCommand row until its ack fields are populated, or time out.
    Returns (dispatch_id, ack_state, ack_timestamp) once ack_state is set."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        # A fresh transaction each poll so we see the server's committed writes.
        db_conn.rollback()
        with db_conn.cursor() as cur:
            cur.execute(
                "SELECT dispatch_id, ack_state, ack_timestamp "
                "FROM assets_assetcommand WHERE id = %s",
                (dbid,),
            )
            row = cur.fetchone()
        if row is not None and row[1] is not None:
            return row
        time.sleep(0.05)
    return None


@pytest.mark.requires_docker
def test_command_ack_is_stored(db_conn, fake_client, server_proc):
    """A dispatched command, once acked by the client, lands its ack on the
    command row: dispatch_id set and ack_state advanced to actioned."""
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]
    db_conn.commit()

    fake_client("test1")

    # Let the client connect, identify, and negotiate the version handshake
    # (the command-ack capability is only negotiated then).
    time.sleep(5)

    with db_conn.cursor() as cur:
        cur.execute(
            "INSERT INTO assets_assetcommand (asset_id, command, position, altitude) "
            "VALUES (%s, 'RTL', ST_SetSRID(ST_MakePoint(0, 0), 4326)::geography, 0) "
            "RETURNING id",
            (asset_id,),
        )
        new_dbid = cur.fetchone()[0]
    db_conn.commit()

    row = _poll_row(db_conn, new_dbid, timeout=10.0)
    assert row is not None, (
        f"command dbid={new_dbid} never got an ack stored; server log:\n"
        + server_proc["log"].read_text(errors="replace")
    )
    dispatch_id, ack_state, ack_timestamp = row
    # The server records the stamped dispatch id so the ack can correlate.
    assert dispatch_id is not None, "dispatch_id was not recorded at dispatch time"
    # The fake client always reports actioned as its terminal outcome; the
    # latest-wins / no-regression rule means the terminal state must win over the
    # earlier received.
    assert ack_state == ACK_STATE_ACTIONED, f"expected ack_state actioned, got {ack_state}"
    assert ack_timestamp is not None and ack_timestamp > 0, "ack_timestamp was not stored"
