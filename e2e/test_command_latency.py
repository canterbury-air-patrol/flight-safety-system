"""todo09: the server polls for new commands every 100ms, so an injected row
must reach the aircraft session in well under a second. This test inserts a
TERM row and asserts the server's "dispatched command" log line appears in
under 1 second — the pre-todo09 loop would have taken up to 1000ms plus the
per-iteration body before sending."""
from __future__ import annotations

import re
import time

import pytest


_DISPATCH_RE = re.compile(r"dispatched command dbid=(\d+)")

# Should match flight_safety_system::server::command_poll_ms in src/fss-server.hpp
COMMAND_POLL_MS = 100


def _wait_for_dispatch(log_path, dbid: int, timeout: float) -> float | None:
    """Poll the server log file for a dispatch line referencing `dbid`.
    Returns seconds waited, or None on timeout."""
    deadline = time.monotonic() + timeout
    start = time.monotonic()
    while time.monotonic() < deadline:
        try:
            text = log_path.read_text(errors="replace")
        except FileNotFoundError:
            text = ""
        for match in _DISPATCH_RE.finditer(text):
            if int(match.group(1)) == dbid:
                return time.monotonic() - start
        time.sleep(0.02)
    return None


@pytest.mark.requires_docker
def test_command_dispatched_under_one_second(db_conn, fake_client, server_proc):
    """Insert a command after the client has identified, then assert the
    server dispatches it in <1s (should be well under — ~100ms)."""
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]

    fake_client("test1")

    # Let the client connect and identify. The server dispatches any existing
    # command on identify, so we wait until identify is done before inserting.
    time.sleep(5)

    # Note how many dispatch lines exist pre-insert; we only care about
    # the dbid returned by the INSERT below.
    with db_conn.cursor() as cur:
        cur.execute(
            "INSERT INTO assets_assetcommand (asset_id, command, position, altitude) "
            "VALUES (%s, 'TERM', "
            "        ST_SetSRID(ST_MakePoint(0, 0), 4326)::geography, 0) "
            "RETURNING id",
            (asset_id,),
        )
        new_dbid = cur.fetchone()[0]

    elapsed = _wait_for_dispatch(server_proc["log"], new_dbid, timeout=3.0)
    assert elapsed is not None, (
        f"server never dispatched command dbid={new_dbid}; log:\n"
        + server_proc["log"].read_text(errors="replace")
    )
    # The poll loop runs every 100ms; allow 1s (or 10x poll) to cover DB
    # round-trip jitter and e2e noise.
    latency_bound = max(1.0, 10 * COMMAND_POLL_MS / 1000.0)
    assert elapsed < latency_bound, f"command dispatch took {elapsed:.3f}s, expected <{latency_bound}s"
