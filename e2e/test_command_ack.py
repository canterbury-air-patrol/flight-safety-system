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


def _wait_for_client_ready(server_proc, name: str, timeout: float = 15.0) -> None:
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
    We dispatch one command to B to learn where its connection's dispatch_id
    currently sits, then pre-seed asset A with a *band* of database-only command
    rows spanning the dispatch_ids B's next command might use, each carrying a
    distinct terminal ack (superseded). dispatch_id is the connection's
    last_msg_id, bumped by every server->client message — not just commands —
    so the once-a-second RTT request can land between B's two commands and B's
    next command is not reliably the very next id. Seeding a band rather than a
    single guessed id makes the collision deterministic: B's second command
    lands on one row in the band, and the asset scoping must leave every seeded
    A row untouched while B's own row advances to actioned.
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
    _wait_for_client_ready(server_proc, "test2")

    # First dispatch to B: learn the dispatch_id its connection is currently at.
    b_first = _dispatch_rtl(db_conn, asset_b)
    b_first_dispatch = _poll_field(db_conn, b_first, "dispatch_id", timeout=10.0)
    assert b_first_dispatch is not None, (
        f"B's first command dbid={b_first} never recorded a dispatch_id; server log:\n"
        + server_proc["log"].read_text(errors="replace")
    )
    # Let B's own ack for this first command settle so it can't interfere later.
    _poll_field(db_conn, b_first, "ack_state", timeout=10.0)

    # Pre-seed asset A with a band of database-only rows spanning the
    # dispatch_ids B's next command might use, each with a terminal superseded
    # ack the fake client never produces — so any change is unambiguous. The
    # band absorbs RTT/other server->client messages that bump B's id between
    # commands by an unpredictable (small) amount; B's second command lands on
    # exactly one of these. The band is wide enough to cover many seconds of
    # intervening once-a-second RTT traffic.
    sentinel_ts = SENTINEL_ACK_TIMESTAMP
    sentinel_reason = SENTINEL_SUPERSEDE_REASON
    collision_band = range(b_first_dispatch + 1, b_first_dispatch + 1 + COLLISION_BAND)
    a_dbids: dict[int, int] = {}  # dispatch_id -> asset A command row id
    with db_conn.cursor() as cur:
        for dispatch_id in collision_band:
            cur.execute(
                "INSERT INTO assets_assetcommand "
                "(asset_id, command, position, altitude, dispatch_id, "
                " ack_state, ack_timestamp, ack_superseded_by) "
                "VALUES (%s, 'RTL', ST_SetSRID(ST_MakePoint(0, 0), 4326)::geography, 0, "
                "        %s, %s, %s, %s) RETURNING id",
                (asset_a, dispatch_id, ACK_STATE_SUPERSEDED, sentinel_ts, sentinel_reason),
            )
            a_dbids[dispatch_id] = cur.fetchone()[0]
    db_conn.commit()

    # Second dispatch to B: lands on one dispatch_id in the band and is acked.
    b_second = _dispatch_rtl(db_conn, asset_b)
    b_second_dispatch = _poll_field(db_conn, b_second, "dispatch_id", timeout=10.0)
    b_second_state = _poll_ack_state(db_conn, b_second, ACK_STATE_ACTIONED, timeout=10.0)

    # Guard the construction itself: if B's command did not collide with a seeded
    # row the test proves nothing, so fail loudly rather than pass vacuously.
    assert b_second_dispatch in collision_band, (
        f"setup failed to collide: B's second command got dispatch_id="
        f"{b_second_dispatch}, outside the seeded band "
        f"{collision_band.start}..{collision_band.stop - 1}; server log:\n"
        + server_proc["log"].read_text(errors="replace")
    )
    assert b_second_state == ACK_STATE_ACTIONED, (
        f"B's own command should have been acked actioned, got {b_second_state}"
    )

    # The real assertion: none of asset A's rows are touched by B's ack — above
    # all the one sharing B's dispatch_id. With the bug (match on dispatch_id
    # alone) B's actioned ack would overwrite that row's superseded sentinel and
    # stamp it with B's ack_timestamp.
    db_conn.rollback()
    with db_conn.cursor() as cur:
        cur.execute(
            "SELECT dispatch_id, ack_state, ack_timestamp, ack_superseded_by "
            "FROM assets_assetcommand WHERE asset_id = %s ORDER BY dispatch_id",
            (asset_a,),
        )
        a_rows = cur.fetchall()
    for dispatch_id, a_state, a_ts, a_reason in a_rows:
        assert (a_state, a_ts, a_reason) == (ACK_STATE_SUPERSEDED, sentinel_ts, sentinel_reason), (
            f"asset A's command row at dispatch_id={dispatch_id} was mutated by asset B's "
            f"ack (B collided at dispatch_id={b_second_dispatch}): got ack_state={a_state}, "
            f"ack_timestamp={a_ts}, ack_superseded_by={a_reason}; server log:\n"
            + server_proc["log"].read_text(errors="replace")
        )


@pytest.mark.requires_docker
def test_ack_updates_only_the_latest_row_on_cross_session_dispatch_id_reuse(
    db_conn, fake_client, server_proc
):
    """Within a single asset, an ack must update only the newest command sharing
    a dispatch_id, not an old already-settled one from a prior session.

    dispatch_id is the connection's last_msg_id, which resets to 0 on every
    reconnect, so the same asset accumulates several historical command rows that
    share a dispatch_id across sessions. The ack is for the command just
    dispatched on the current connection — the latest row — so the UPDATE targets
    the newest match. An ack must never reach back and rewrite a long-settled
    historical row that happens to carry the same dispatch_id.

    We give the asset one live client, seed an OLD command row with a terminal
    "superseded" sentinel and an old timestamp at the dispatch_id the next live
    command will use, then dispatch a NEW command that lands on that same
    dispatch_id. The client's ack must advance only the NEW row and leave the OLD
    sentinel untouched.
    """
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset = cur.fetchone()[0]
    db_conn.commit()

    fake_client("test1")
    _wait_for_client_ready(server_proc, "test1")

    # First dispatch: learn the connection's current dispatch_id.
    first = _dispatch_rtl(db_conn, asset)
    first_dispatch = _poll_field(db_conn, first, "dispatch_id", timeout=10.0)
    assert first_dispatch is not None, (
        f"first command dbid={first} never recorded a dispatch_id; server log:\n"
        + server_proc["log"].read_text(errors="replace")
    )
    _poll_field(db_conn, first, "ack_state", timeout=10.0)

    # Seed OLD already-acked rows for the SAME asset across the band of
    # dispatch_ids the next live command might land on — the connection's
    # last_msg_id is bumped by RTT and other server->client traffic, so it is
    # not reliably first_dispatch + 1. Each carries an explicitly older timestamp
    # so the newest-row subselect must prefer the live command's row, never these.
    sentinel_ts = SENTINEL_ACK_TIMESTAMP
    sentinel_reason = SENTINEL_SUPERSEDE_REASON
    collision_band = range(first_dispatch + 1, first_dispatch + 1 + COLLISION_BAND)
    old_dbids: dict[int, int] = {}  # dispatch_id -> seeded historical row id
    with db_conn.cursor() as cur:
        for dispatch_id in collision_band:
            cur.execute(
                "INSERT INTO assets_assetcommand "
                "(asset_id, command, position, altitude, timestamp, dispatch_id, "
                " ack_state, ack_timestamp, ack_superseded_by) "
                "VALUES (%s, 'RTL', ST_SetSRID(ST_MakePoint(0, 0), 4326)::geography, 0, "
                "        NOW() - INTERVAL '1 hour', %s, %s, %s, %s) RETURNING id",
                (asset, dispatch_id, ACK_STATE_SUPERSEDED, sentinel_ts, sentinel_reason),
            )
            old_dbids[dispatch_id] = cur.fetchone()[0]
    db_conn.commit()

    # New dispatch: lands on one dispatch_id in the band with a fresh timestamp.
    new_dbid = _dispatch_rtl(db_conn, asset)
    new_dispatch = _poll_field(db_conn, new_dbid, "dispatch_id", timeout=10.0)
    new_state = _poll_ack_state(db_conn, new_dbid, ACK_STATE_ACTIONED, timeout=10.0)

    assert new_dispatch in collision_band, (
        f"setup failed to reuse dispatch_id: the new command got dispatch_id="
        f"{new_dispatch}, outside the seeded band "
        f"{collision_band.start}..{collision_band.stop - 1}; server log:\n"
        + server_proc["log"].read_text(errors="replace")
    )
    # The new (latest) row is the one the ack must land on.
    assert new_state == ACK_STATE_ACTIONED, (
        f"the new command should have been acked actioned, got {new_state}"
    )

    # The old, already-settled rows must all be untouched — above all the one
    # sharing the new command's dispatch_id, which the newest-row subselect must
    # not reach back to.
    db_conn.rollback()
    with db_conn.cursor() as cur:
        cur.execute(
            "SELECT dispatch_id, ack_state, ack_timestamp, ack_superseded_by "
            "FROM assets_assetcommand WHERE id = ANY(%s) ORDER BY dispatch_id",
            (list(old_dbids.values()),),
        )
        old_rows = cur.fetchall()
    for dispatch_id, old_state, old_ts, old_reason in old_rows:
        assert (old_state, old_ts, old_reason) == (ACK_STATE_SUPERSEDED, sentinel_ts, sentinel_reason), (
            f"the old already-acked row at dispatch_id={dispatch_id} was rewritten by an ack "
            f"meant for the newer command (which landed at dispatch_id={new_dispatch}): got "
            f"ack_state={old_state}, ack_timestamp={old_ts}, ack_superseded_by={old_reason}; "
            "server log:\n" + server_proc["log"].read_text(errors="replace")
        )
