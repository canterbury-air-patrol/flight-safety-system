"""End-to-end: a stalled telemetry write must not block command delivery.

The server uses two independent database connections — one for telemetry
writes (positions, RTT, status) and one for command/config reads. Holding an
EXCLUSIVE lock on assets_assetposition stalls the write connection on its next
position INSERT. A command inserted into the DB *while that write is stalled*
must still be picked up by the command poller (read connection) and delivered
to the connected client.

Before the read/write connection split, both shared one connection guarded by
one mutex, so a stalled write blocked the command read and this command would
not arrive until the lock was released. This test fails against that design
and passes once reads and writes are decoupled.

The lock helper is shared with test_slow_db_does_not_stall to avoid
duplicating the EXCLUSIVE-lock plumbing.
"""
from __future__ import annotations

import time

import pytest

from test_slow_db_does_not_stall import hold_table_lock


@pytest.mark.requires_docker
@pytest.mark.slow
def test_command_read_not_blocked_by_write_stall(db_conn, fake_client, migrated_db, server_proc):
    """A command queued during a telemetry-write stall must still reach the
    client, proving the command read path is not serialized behind writes."""
    # Must match a generated client cert CN (certs_dir issues test1/test2/test3);
    # the server authenticates the client by matching its cert CN to the asset.
    asset_name = "test1"
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES (%s) RETURNING id", (asset_name,))
        asset_id = cur.fetchone()[0]

    client = fake_client(asset_name)
    log_path = client["log"]

    # Let the client connect, identify, and start sending position reports so
    # the writer has telemetry to flush (and thus a write to stall on).
    time.sleep(3.0)
    assert client["proc"].poll() is None, "client exited before the stall test"

    cmd_marker = "RTL"
    with hold_table_lock(migrated_db, "assets_assetposition"):
        # Give the writer time to dequeue a position report and block on the
        # EXCLUSIVE lock — the telemetry write is now stalled and holding the
        # write connection.
        time.sleep(2.0)

        # Queue a command while the write is stalled.
        with db_conn.cursor() as cur:
            cur.execute(
                "INSERT INTO assets_assetcommand (asset_id, command, timestamp, position, altitude) "
                "VALUES (%s, 'RTL', NOW(), ST_SetSRID(ST_MakePoint(0,0),4326), 0)",
                (asset_id,),
            )

        # The command must reach the client *while the write lock is still
        # held*. The poll budget stays inside the lock's hold window.
        found = False
        deadline = time.monotonic() + 8.0
        while time.monotonic() < deadline:
            with open(log_path, "r", encoding="utf-8") as fh:
                if cmd_marker in fh.read():
                    found = True
                    break
            time.sleep(0.2)

        assert found, (
            "command was not delivered while a telemetry write was stalled — "
            "the command read path appears blocked behind writes"
        )

        # Both processes must still be alive during the stall.
        assert server_proc["proc"].poll() is None, "server died during the write stall"
        assert client["proc"].poll() is None, "client died during the write stall"
