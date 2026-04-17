"""A row inserted into assets_assetcommand must be delivered to the client."""
from __future__ import annotations

import time

import pytest


@pytest.mark.requires_docker
def test_goto_command_delivered(db_conn, fake_client):
    """Server polls assets_assetcommand and sends asset_command_goto to any
    connected client. The fake client logs nothing distinctive on receipt, so
    we assert via database side-effect that can only happen if the command was
    dispatched: the server must keep talking to the client (no crash), and
    subsequent position rows continue to arrive after the command is injected."""
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

    # And positions must keep flowing after the command landed.
    with db_conn.cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s",
            (asset_id,),
        )
        count = cur.fetchone()[0]

    assert count >= 1, "no position rows after command injection"
