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
from pathlib import Path

import pytest

from conftest import wait_for_row
from test_slow_db_does_not_stall import hold_table_lock

# The asset command to inject; the fake client logs the command name on
# receipt, so the same string drives the SQL INSERT and the log assertion.
COMMAND = "RTL"

# Time budgets (seconds), kept generous so the test is not brittle on slower
# CI. WARMUP and COMMAND_DELIVERY are polled, not slept; WRITER_SETTLE is the
# one short fixed wait — long enough for a position INSERT to reach the locked
# table so a write is genuinely in flight (the write path being blocked cannot
# be observed precisely from the test side).
CLIENT_WARMUP_TIMEOUT_S = 15.0
WRITER_SETTLE_S = 2.0
COMMAND_DELIVERY_TIMEOUT_S = 8.0


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
    log_path = Path(client["log"])

    # Poll until the client has connected, identified, and landed at least one
    # position row — proof it is up and the writer has telemetry to stall on.
    baseline = wait_for_row(
        db_conn,
        "SELECT id FROM assets_assetposition WHERE asset_id = %s LIMIT 1",
        params=(asset_id,),
        timeout=CLIENT_WARMUP_TIMEOUT_S,
    )
    assert baseline is not None, "client never produced a baseline position row"
    assert client["proc"].poll() is None, "client exited before the stall test"

    with hold_table_lock(migrated_db, "assets_assetposition"):
        # Let a position INSERT reach the now-locked table so a telemetry write
        # is genuinely stalled (holding the write connection).
        time.sleep(WRITER_SETTLE_S)

        # Queue a command while the write is stalled.
        with db_conn.cursor() as cur:
            cur.execute(
                "INSERT INTO assets_assetcommand (asset_id, command, timestamp, position, altitude) "
                "VALUES (%s, %s, NOW(), ST_SetSRID(ST_MakePoint(0,0),4326), 0)",
                (asset_id, COMMAND),
            )

        # The command must reach the client *while the write lock is still
        # held*. The poll budget stays inside the lock's hold window.
        deadline = time.monotonic() + COMMAND_DELIVERY_TIMEOUT_S
        found = False
        while time.monotonic() < deadline:
            if COMMAND in log_path.read_text(errors="replace"):
                found = True
                break
            time.sleep(0.2)

        if not found:
            # Capture log context to make timing-sensitive CI flakes diagnosable.
            tail = 2000
            client_log = log_path.read_text(errors="replace")
            server_log = Path(server_proc["log"]).read_text(errors="replace")
            pytest.fail(
                "command was not delivered while a telemetry write was stalled — "
                "the command read path appears blocked behind writes.\n"
                f"--- client log (last {tail} chars) ---\n{client_log[-tail:]}\n"
                f"--- server log (last {tail} chars) ---\n{server_log[-tail:]}"
            )

        # Both processes must still be alive during the stall.
        assert server_proc["proc"].poll() is None, "server died during the write stall"
        assert client["proc"].poll() is None, "client died during the write stall"
