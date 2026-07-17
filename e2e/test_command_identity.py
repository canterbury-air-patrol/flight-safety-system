"""e2e: dispatched commands carry the server's command row id (todo/49).

fss_message_asset_command gains an optional server_command_id, negotiated
via FSS_FEATURE_SERVER_COMMAND_ID: the dispatching server's DB row id for
the operator action. Within one connection the id identifies the action --
a redelivery reuses it (pinned across a server bounce in
test_server_restart), while a deliberate operator retry (a NEW row with
identical command/payload) arrives with a fresh id. That per-server
identity is what lets an FMU connected to redundant servers (cap-fmu
todo/86) tell a genuine retry apart from another server's delivery of the
same push. Ids are unique per server only and must never be compared
across connections.
"""
from __future__ import annotations

import re
import time
from pathlib import Path

import pytest

CMD_ID_RE = re.compile(r"RCVD_CMD: RTL cmd_id=(\d+)")


def _delivered_ids(log_path: Path) -> list[int]:
    return [int(m) for m in CMD_ID_RE.findall(log_path.read_text(errors="replace"))]


def _wait_for_id(log_path: Path, expected_id: int, timeout: float = 15.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if expected_id in _delivered_ids(log_path):
            return True
        time.sleep(0.1)
    return False


@pytest.mark.requires_docker
def test_command_id_matches_row_and_retry_gets_fresh_id(db_conn, fake_client, server_proc):
    """The wire id equals the inserted row's id, and an operator retry --
    a second row identical in command and payload -- is delivered with the
    new row's id, not swallowed as a duplicate of the first."""
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]

    client = fake_client("test1")
    log = client["log"]

    def insert_rtl() -> int:
        with db_conn.cursor() as cur:
            cur.execute(
                "INSERT INTO assets_assetcommand (asset_id, command, position, altitude) "
                "VALUES (%s, 'RTL', ST_SetSRID(ST_MakePoint(0, 0), 4326)::geography, 0) "
                "RETURNING id",
                (asset_id,),
            )
            return cur.fetchone()[0]

    first_id = insert_rtl()
    assert _wait_for_id(log, first_id), (
        f"command row {first_id} was not delivered with its own id as cmd_id\n"
        + log.read_text(errors="replace")
    )

    # The retry: a new row, byte-identical command/payload/altitude. Only the
    # row id (and its DB timestamp) differ -- exactly the delivery cap-fmu's
    # dedup window used to swallow. It must arrive under the fresh id.
    second_id = insert_rtl()
    assert second_id != first_id
    assert _wait_for_id(log, second_id), (
        f"identical-payload retry row {second_id} was not delivered with a fresh cmd_id "
        f"(first row was {first_id})\n" + log.read_text(errors="replace")
    )

    # Every delivery carried a real id from the rows we created: never 0
    # (unreported -- both binaries are this build, so the capability is
    # always negotiated) and never a value we did not insert.
    ids = _delivered_ids(log)
    assert set(ids) <= {first_id, second_id}, f"unexpected cmd_id values delivered: {ids}"
