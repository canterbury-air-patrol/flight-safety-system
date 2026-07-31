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
ACK_STATE_SUPERSEDED = 2

# Mirror flight_safety_system::transport::fss_command_ack_reason, the codes
# stored in ack_superseded_by. supersede_newer_command is the only reason the
# fake client could ever name; we use it as the sentinel reason precisely
# because the fake client never supersedes its own command, so the live ack
# path under test cannot produce it.
SUPERSEDE_NEWER_COMMAND = 3

# A pre-seeded "already superseded" row carries this fixed ack timestamp and
# reason. Neither value is produced by the live ack path under test, so any
# change to a seeded row is unambiguously attributable to the code under test.
SENTINEL_ACK_TIMESTAMP = 111
SENTINEL_SUPERSEDE_REASON = SUPERSEDE_NEWER_COMMAND

# A connection's dispatch_id (last_msg_id) is bumped by every server->client
# message, so the once-a-second RTT request can fall between two commands and
# the next command's id is not reliably +1. The cross-asset collision test
# seeds asset A across this many candidate dispatch_ids so B's next command
# reliably collides with one — far wider than any realistic test window's worth
# of intervening once-a-second traffic.
COLLISION_BAND = 20

# Nothing retires a command row, so the newest row for an asset stays pending
# and sendCommand() re-dispatches it every 10 s (client_session.cpp's
# `timeout_time`, the resend window).
#
# A redelivery used to null ack_state, ack_timestamp and ack_superseded_by
# before the fresh ack re-populated them, so each one opened a brief window in
# which the ack columns read NULL for a command that had been acked. Polling on
# exactly the resend window put every poll on that boundary and could time out
# inside the gap, reporting "never acked" — hence the deliberately off-boundary
# deadline below.
#
# todo/68 closed the gap at the source: a resend no longer records a dispatch,
# so the ack columns are written once per delivery and never cleared under a
# poll. The margin is kept anyway, because it costs nothing on the happy path
# (every poll returns as soon as its condition holds) and the polls still have
# to outlast a first delivery that lands just after a poll starts.
RESEND_WINDOW = 10.0
ACK_POLL_TIMEOUT = 2.5 * RESEND_WINDOW

# How long _wait_for_client_ready is given, repeated here so the per-test budget
# below can be derived rather than guessed.
CLIENT_READY_TIMEOUT = 15.0

# pytest.ini caps every test at 60 s, which is the reason the poll deadline used
# to be exactly the resend window: the dispatch_id-collision test makes four
# sequential polls, and 15 + 4x10 = 55 s just fit under the cap. Raising the
# polls off the boundary breaks that fit, and a pytest-timeout kill is a strictly
# worse failure than a poll timeout — it loses the assertion messages, which
# carry the server log. So give that test a budget derived from the polls it
# actually makes. Only the worst case is longer; the happy path is unchanged,
# since every poll returns as soon as its condition holds.
COLLISION_TEST_TIMEOUT = CLIENT_READY_TIMEOUT + 4 * ACK_POLL_TIMEOUT + 30.0


def _wait_for_client_ready(server_proc, name: str, timeout: float = CLIENT_READY_TIMEOUT) -> None:
    """Block until the server reports that the aircraft client `name` identified.

    The fake client connects, runs the protocol-version handshake (which is when
    the command-ack capability is negotiated) and only then identifies; the
    server logs "Aircraft client identified: <name>" at that point. Waiting for
    that line is a precise readiness signal — the connection exists and the
    capability is negotiated — and replaces a blind fixed sleep that was both
    slower and prone to flaking under load.

    Returns once the line is seen; raises AssertionError with the full server log
    on timeout, so callers need not repeat that check. The log is tailed from the
    last read offset each poll rather than re-read whole, so the cost does not
    grow with the log."""
    needle = f"Aircraft client identified: {name}".encode()
    log_path = server_proc["log"]
    deadline = time.monotonic() + timeout
    seen = b""
    offset = 0
    while time.monotonic() < deadline:
        with log_path.open("rb") as fp:
            fp.seek(offset)
            seen += fp.read()
            offset = fp.tell()
        if needle in seen:
            return
        time.sleep(0.05)
    raise AssertionError(
        f"client {name} never identified within {timeout:.0f}s; server log:\n"
        + seen.decode(errors="replace")
    )


def _poll(db_conn, query: str, params: tuple, want, timeout: float, poll: float = 0.05):
    """Poll `query` until `want(row)` holds for the fetched row, or time out.

    A fresh transaction is taken each iteration so the server's committed async
    writes become visible. Returns the last row fetched (so a timeout still
    reports what was stored), or None if the query never returned a row."""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        db_conn.rollback()
        with db_conn.cursor() as cur:
            cur.execute(query, params)
            row = cur.fetchone()
        if row is not None:
            last = row
            if want(row):
                return row
        time.sleep(poll)
    return last


def _poll_row(db_conn, dbid: int, timeout: float, expected_state: int = ACK_STATE_ACTIONED):
    """Poll the AssetCommand row until ack_state reaches the terminal
    `expected_state`, or time out.

    The ack is two-phase: the client sends `received` (0) immediately followed
    by `actioned` (1), persisted as two async writes. Returning as soon as
    ack_state is merely non-NULL races those writes and can latch the transient
    `received`; callers assert the terminal outcome, so wait for it. Returns
    (dispatch_id, ack_state, ack_timestamp) once ack_state is set, else None."""
    row = _poll(
        db_conn,
        "SELECT dispatch_id, ack_state, ack_timestamp "
        "FROM assets_assetcommand WHERE id = %s",
        (dbid,),
        lambda r: r[1] == expected_state,
        timeout,
    )
    return row if row is not None and row[1] is not None else None


def _poll_field(db_conn, dbid: int, field: str, timeout: float):
    """Poll a single non-NULL column of an AssetCommand row, or time out.

    `field` is a fixed identifier chosen by the test (never request data), so
    interpolating it into the query is safe here."""
    assert field in {"dispatch_id", "ack_state"}, field
    row = _poll(
        db_conn,
        f"SELECT {field} FROM assets_assetcommand WHERE id = %s",  # noqa: S608
        (dbid,),
        lambda r: r[0] is not None,
        timeout,
        poll=0.02,
    )
    return row[0] if row is not None else None


def _poll_ack_state(db_conn, dbid: int, expected: int, timeout: float):
    """Poll a row's ack_state until it reaches the terminal `expected` value, or
    time out. See _poll_row for why a bare non-NULL check races the two-phase
    ack. Returns the last non-NULL state seen (so a timeout reports what was
    stored), or None if no ack ever landed."""
    row = _poll(
        db_conn,
        "SELECT ack_state FROM assets_assetcommand WHERE id = %s",
        (dbid,),
        lambda r: r[0] == expected,
        timeout,
        poll=0.02,
    )
    return row[0] if row is not None else None


@pytest.mark.satisfies("TC-SRV-004")
@pytest.mark.requires_docker
def test_command_ack_is_stored(db_conn, fake_client, server_proc):
    """A dispatched command, once acked by the client, lands its ack on the
    command row: dispatch_id set and ack_state advanced to actioned."""
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]
    db_conn.commit()

    fake_client("test1")

    # Wait for the client to connect, negotiate the handshake (where the
    # command-ack capability is agreed) and identify, rather than blind-sleeping.
    _wait_for_client_ready(server_proc, "test1")

    with db_conn.cursor() as cur:
        cur.execute(
            "INSERT INTO assets_assetcommand (asset_id, command, position, altitude) "
            "VALUES (%s, 'RTL', ST_SetSRID(ST_MakePoint(0, 0), 4326)::geography, 0) "
            "RETURNING id",
            (asset_id,),
        )
        new_dbid = cur.fetchone()[0]
    db_conn.commit()

    row = _poll_row(db_conn, new_dbid, timeout=ACK_POLL_TIMEOUT)
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
    assert ack_timestamp is not None, "ack_timestamp was not stored"
    assert ack_timestamp > 0, f"ack_timestamp should be positive, got {ack_timestamp}"


def _dispatch_rtl(db_conn, asset_id: int) -> int:
    """Insert an RTL command for the asset (which the server will dispatch on its
    next poll) and return the new command row's id."""
    with db_conn.cursor() as cur:
        cur.execute(
            "INSERT INTO assets_assetcommand (asset_id, command, position, altitude) "
            "VALUES (%s, 'RTL', ST_SetSRID(ST_MakePoint(0, 0), 4326)::geography, 0) "
            "RETURNING id",
            (asset_id,),
        )
        new_dbid = cur.fetchone()[0]
    db_conn.commit()
    return new_dbid




@pytest.mark.satisfies("TC-SRV-004")
@pytest.mark.requires_docker
@pytest.mark.timeout(COLLISION_TEST_TIMEOUT)
def test_ack_lands_only_on_the_dispatched_row_despite_dispatch_id_collisions(
    db_conn, fake_client, server_proc
):
    """An ack must reach only the command row it was dispatched for, whatever
    else shares its dispatch_id.

    dispatch_id is the connection's last_msg_id: it is unique to neither the
    asset nor the session (it restarts at 0 on every connection), so rows
    sharing one are routine in both directions — another asset's row, and this
    asset's own older row from a previous session.

    Since todo/68 the ack is keyed on the command row's primary key, which the
    dispatching session resolved locally, so a collision cannot reach the wrong
    row. This test replaces the two that pinned the older reconstruction (an
    (asset_id, dispatch_id) match plus an `ORDER BY timestamp DESC LIMIT 1`
    subselect); it covers both of their regressions in one pass and, unlike
    them, keeps holding if the row-id keying is ever reverted.

    Construction (no hard-coded dispatch_id): dispatch one command to the live
    asset to learn where its connection's dispatch_id sits, then seed a *band*
    spanning the ids the next command might use — the once-a-second RTT request
    also bumps last_msg_id, so the next command is not reliably +1. Each seeded
    row carries a terminal `superseded` sentinel the fake client never produces,
    so any change to one is unambiguously attributable to the code under test.
    """
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        live_asset = cur.fetchone()[0]
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test2') RETURNING id")
        other_asset = cur.fetchone()[0]
    db_conn.commit()

    fake_client("test1")
    _wait_for_client_ready(server_proc, "test1")

    # First dispatch: learn the connection's current dispatch_id, and let its
    # own ack settle so it cannot interfere with the assertions below.
    first = _dispatch_rtl(db_conn, live_asset)
    first_dispatch = _poll_field(db_conn, first, "dispatch_id", timeout=ACK_POLL_TIMEOUT)
    assert first_dispatch is not None, (
        f"first command dbid={first} never recorded a dispatch_id; server log:\n"
        + server_proc["log"].read_text(errors="replace")
    )
    _poll_field(db_conn, first, "ack_state", timeout=ACK_POLL_TIMEOUT)

    # Seed both collision shapes across the band: a *different asset's* row
    # (which an unscoped match would clobber) and this asset's own *older*
    # row from a notional earlier session (which a newest-row subselect could
    # reach back to if it picked wrong).
    sentinel_ts = SENTINEL_ACK_TIMESTAMP
    sentinel_reason = SENTINEL_SUPERSEDE_REASON
    collision_band = range(first_dispatch + 1, first_dispatch + 1 + COLLISION_BAND)
    seeded: list[int] = []
    with db_conn.cursor() as cur:
        for dispatch_id in collision_band:
            # Both are timestamped an hour ago: the same-asset row has to be
            # older than the live dispatch for the historical-row case to mean
            # anything, and the other asset's age is immaterial.
            for asset_id in (other_asset, live_asset):
                cur.execute(
                    "INSERT INTO assets_assetcommand "
                    "(asset_id, command, position, altitude, timestamp, dispatch_id, "
                    " ack_state, ack_timestamp, ack_superseded_by) "
                    "VALUES (%s, 'RTL', ST_SetSRID(ST_MakePoint(0, 0), 4326)::geography, 0, "
                    "        NOW() - INTERVAL '1 hour', %s, %s, %s, %s) RETURNING id",
                    (asset_id, dispatch_id, ACK_STATE_SUPERSEDED, sentinel_ts, sentinel_reason),
                )
                seeded.append(cur.fetchone()[0])
    db_conn.commit()

    # The live dispatch: lands on one dispatch_id in the band, colliding with
    # two seeded rows, and is acked by the client.
    new_dbid = _dispatch_rtl(db_conn, live_asset)
    new_dispatch = _poll_field(db_conn, new_dbid, "dispatch_id", timeout=ACK_POLL_TIMEOUT)
    new_state = _poll_ack_state(db_conn, new_dbid, ACK_STATE_ACTIONED, timeout=ACK_POLL_TIMEOUT)

    # Guard the construction itself: without a collision the test proves
    # nothing, so fail loudly rather than pass vacuously.
    assert new_dispatch in collision_band, (
        f"setup failed to collide: the new command got dispatch_id={new_dispatch}, "
        f"outside the seeded band {collision_band.start}..{collision_band.stop - 1}; "
        "server log:\n" + server_proc["log"].read_text(errors="replace")
    )
    assert new_state == ACK_STATE_ACTIONED, (
        f"the dispatched command should have been acked actioned, got {new_state}"
    )

    # The real assertion: every seeded row still carries its sentinel.
    db_conn.rollback()
    with db_conn.cursor() as cur:
        cur.execute(
            "SELECT id, asset_id, dispatch_id, ack_state, ack_timestamp, ack_superseded_by "
            "FROM assets_assetcommand WHERE id = ANY(%s) ORDER BY id",
            (seeded,),
        )
        rows = cur.fetchall()
    assert len(rows) == len(seeded), f"expected {len(seeded)} seeded rows, read {len(rows)}"
    for row_id, asset_id, dispatch_id, state, ack_ts, reason in rows:
        which = "the other asset's" if asset_id == other_asset else "this asset's older"
        assert (state, ack_ts, reason) == (ACK_STATE_SUPERSEDED, sentinel_ts, sentinel_reason), (
            f"{which} command row id={row_id} at dispatch_id={dispatch_id} was mutated by an "
            f"ack meant for dbid={new_dbid} (which landed at dispatch_id={new_dispatch}): got "
            f"ack_state={state}, ack_timestamp={ack_ts}, ack_superseded_by={reason}; "
            "server log:\n" + server_proc["log"].read_text(errors="replace")
        )
