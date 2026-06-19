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


def _poll_field(db_conn, dbid: int, field: str, timeout: float):
    """Poll a single non-NULL column of an AssetCommand row, or time out.

    `field` is a fixed identifier chosen by the test (never request data), so
    interpolating it into the query is safe here."""
    assert field in {"dispatch_id", "ack_state"}, field
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        db_conn.rollback()
        with db_conn.cursor() as cur:
            cur.execute(
                f"SELECT {field} FROM assets_assetcommand WHERE id = %s",  # noqa: S608
                (dbid,),
            )
            row = cur.fetchone()
        if row is not None and row[0] is not None:
            return row[0]
        time.sleep(0.02)
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


@pytest.mark.requires_docker
def test_ack_does_not_cross_assets_on_dispatch_id_collision(
    db_conn, fake_client, server_proc
):
    """An ack from one asset must not touch another asset's command row, even
    when the two rows share a dispatch_id.

    dispatch_id is a per-connection monotonic id, not globally unique, so two
    assets routinely have commands stamped with the same dispatch_id. The ack
    UPDATE is scoped by asset_id (not dispatch_id alone); without that scoping an
    ack for asset B would overwrite asset A's same-dispatch_id row and show the
    operator a false confirmation on A. This is the regression the single-asset
    test could not catch.

    Construction (no hard-coded dispatch_id): only asset B has a live client.
    We dispatch one command to B to learn its connection's dispatch_id, then
    pre-seed asset A's command row at the *next* dispatch_id with a distinct
    terminal ack (superseded) and no live client of its own. Dispatching a second
    command to B lands on that same dispatch_id (per-connection ids are
    contiguous), so B's ack collides with A's row. The asset scoping must leave
    A's superseded sentinel untouched while B's own row advances to actioned.
    """
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_a = cur.fetchone()[0]
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test2') RETURNING id")
        asset_b = cur.fetchone()[0]
    db_conn.commit()

    # Only asset B gets a live client; asset A is a database-only asset whose
    # command row we craft to collide with B's dispatch_id.
    fake_client("test2")
    time.sleep(5)

    # First dispatch to B: learn the dispatch_id its connection is currently at.
    b_first = _dispatch_rtl(db_conn, asset_b)
    b_first_dispatch = _poll_field(db_conn, b_first, "dispatch_id", timeout=10.0)
    assert b_first_dispatch is not None, (
        f"B's first command dbid={b_first} never recorded a dispatch_id; server log:\n"
        + server_proc["log"].read_text(errors="replace")
    )
    # Let B's own ack for this first command settle so it can't interfere later.
    _poll_field(db_conn, b_first, "ack_state", timeout=10.0)

    # Pre-seed asset A's command at the dispatch_id B's *next* command will use
    # (per-connection ids are contiguous), with a terminal superseded ack that
    # the fake client never produces — so any change to it is unambiguous.
    collide_dispatch = b_first_dispatch + 1
    sentinel_ts = 111
    sentinel_reason = 3
    with db_conn.cursor() as cur:
        cur.execute(
            "INSERT INTO assets_assetcommand "
            "(asset_id, command, position, altitude, dispatch_id, "
            " ack_state, ack_timestamp, ack_superseded_by) "
            "VALUES (%s, 'RTL', ST_SetSRID(ST_MakePoint(0, 0), 4326)::geography, 0, "
            "        %s, %s, %s, %s) RETURNING id",
            (asset_a, collide_dispatch, ACK_STATE_SUPERSEDED, sentinel_ts, sentinel_reason),
        )
        a_dbid = cur.fetchone()[0]
    db_conn.commit()

    # Second dispatch to B: should land on collide_dispatch and be acked by B.
    b_second = _dispatch_rtl(db_conn, asset_b)
    b_second_dispatch = _poll_field(db_conn, b_second, "dispatch_id", timeout=10.0)
    b_second_state = _poll_field(db_conn, b_second, "ack_state", timeout=10.0)

    # Guard the construction itself: if the collision didn't actually happen the
    # test proves nothing, so fail loudly rather than pass vacuously.
    assert b_second_dispatch == collide_dispatch, (
        f"setup failed to collide: A.dispatch_id={collide_dispatch} but B's second "
        f"command got dispatch_id={b_second_dispatch}"
    )
    assert b_second_state == ACK_STATE_ACTIONED, (
        f"B's own command should have been acked actioned, got {b_second_state}"
    )

    # The real assertion: asset A's row is untouched by B's ack. With the bug
    # (match on dispatch_id alone), B's actioned ack would overwrite A's
    # superseded sentinel and stamp it with B's ack_timestamp.
    db_conn.rollback()
    with db_conn.cursor() as cur:
        cur.execute(
            "SELECT ack_state, ack_timestamp, ack_superseded_by "
            "FROM assets_assetcommand WHERE id = %s",
            (a_dbid,),
        )
        a_state, a_ts, a_reason = cur.fetchone()
    assert (a_state, a_ts, a_reason) == (ACK_STATE_SUPERSEDED, sentinel_ts, sentinel_reason), (
        "asset A's command row was mutated by asset B's ack despite the shared "
        f"dispatch_id={collide_dispatch}: got ack_state={a_state}, "
        f"ack_timestamp={a_ts}, ack_superseded_by={a_reason}; server log:\n"
        + server_proc["log"].read_text(errors="replace")
    )
