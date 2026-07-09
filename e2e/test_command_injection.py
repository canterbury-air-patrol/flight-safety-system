"""A row inserted into assets_assetcommand must be delivered to the client."""
from __future__ import annotations

import time

import pytest


@pytest.mark.satisfies("TC-SRV-004")
@pytest.mark.requires_docker
def test_goto_command_delivered(db_conn, fake_client):
    """Server polls assets_assetcommand and sends asset_command_goto to any
    connected client. Asserts the full path: DB row -> server -> wire ->
    client parser -> handleCommand, verified by the RCVD_CMD log line."""
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]

    client = fake_client("test1")

    # Let the client connect and send an initial position.
    time.sleep(7)

    with db_conn.cursor() as cur:
        cur.execute(
            "INSERT INTO assets_assetcommand (asset_id, command, position, altitude) "
            "VALUES (%s, 'GOTO', "
            "        ST_SetSRID(ST_MakePoint(172.6, -43.6), 4326)::geography, 500)",
            (asset_id,),
        )

    # Give the server a poll cycle to pick up the command and send it.
    time.sleep(6)

    # The client process must still be alive — commands should not crash it.
    assert client["proc"].poll() is None, (
        "fake-client exited after command injection:\n"
        + client["log"].read_text(errors="replace")
    )

    log_content = client["log"].read_text(errors="replace")

    # Verify the full delivery path: the client must have received, parsed,
    # and handled the command (logged by the handleCommand override).
    assert "RCVD_CMD: GOTO" in log_content, (
        "fake-client did not log RCVD_CMD: GOTO — command may not have been "
        "delivered or parsed correctly.\nClient log:\n" + log_content
    )

    # Positions must keep flowing after the command landed.
    with db_conn.cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s",
            (asset_id,),
        )
        count = cur.fetchone()[0]

    assert count >= 1, "no position rows after command injection"
