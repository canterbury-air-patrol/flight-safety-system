"""End-to-end: a stalled DB must not stop the server from processing messages.

Strategy: hold an EXCLUSIVE lock on assets_assetposition from an independent
psycopg2 connection. While held, the server's async writer thread blocks on
its position INSERT, but the per-client recv thread must continue to drain
the socket (enqueue is non-blocking). After releasing the lock the queue
must drain and new rows must land.
"""
from __future__ import annotations

import time
from collections.abc import Iterator
from contextlib import contextmanager

import psycopg2
import pytest

from conftest import wait_for_row

_LOCKABLE_TABLES = frozenset({"assets_assetposition"})


@contextmanager
def hold_table_lock(db_info: dict[str, object], table: str) -> Iterator[None]:
    """Hold an EXCLUSIVE lock on `table` via a fresh connection.

    EXCLUSIVE blocks INSERT/UPDATE/DELETE from other transactions while
    still permitting SELECT, so the test can continue to observe state.
    """
    if table not in _LOCKABLE_TABLES:
        raise ValueError(f"table {table!r} is not in the allowed list")
    conn = psycopg2.connect(
        host=db_info["host"], port=db_info["port"],
        user=db_info["user"], password=db_info["password"],
        dbname=db_info["dbname"],
    )
    conn.autocommit = False
    try:
        with conn.cursor() as cur:
            cur.execute(f"LOCK TABLE {table} IN EXCLUSIVE MODE")  # noqa: S608 — table validated above
        yield
    finally:
        conn.rollback()
        conn.close()


@pytest.mark.satisfies("TC-SRV-007")
@pytest.mark.requires_docker
@pytest.mark.slow
def test_slow_db_does_not_stall(db_conn, fake_client, migrated_db, server_proc):
    """With assets_assetposition locked, the server must stay responsive and
    new position rows must land once the lock releases."""
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]

    client = fake_client("test1")

    # Baseline: at least one position row lands before we interfere.
    first_row = wait_for_row(
        db_conn,
        "SELECT id FROM assets_assetposition WHERE asset_id = %s LIMIT 1",
        params=(asset_id,),
        timeout=15.0,
    )
    assert first_row is not None, "no baseline position row before lock"

    with db_conn.cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s",
            (asset_id,),
        )
        count_before_lock = cur.fetchone()[0]

    # Hold the lock long enough for the server to buffer several position
    # reports in its write queue.
    lock_hold_s = 12
    with hold_table_lock(migrated_db, "assets_assetposition"):
        time.sleep(lock_hold_s)

        # During the lock, the writer is blocked — no new rows should have
        # landed. The recv thread, however, is not blocked; it must keep
        # enqueuing (verified below by the post-release row count jump).
        with db_conn.cursor() as cur:
            cur.execute(
                "SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s",
                (asset_id,),
            )
            count_during_lock = cur.fetchone()[0]
        assert count_during_lock == count_before_lock, (
            f"rows landed despite EXCLUSIVE lock: before={count_before_lock} "
            f"during={count_during_lock}"
        )

        # Server must still be alive (recv thread was not blocked on DB).
        assert server_proc["proc"].poll() is None, "server died during DB stall"
        assert client["proc"].poll() is None, "client died during DB stall"

    # Lock released. Poll for the queue to drain — the invariant is that
    # buffered writes make it to the DB (some drops are acceptable under a
    # smaller queue depth, but here depth is the default 10000).
    row = wait_for_row(
        db_conn,
        "SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s HAVING COUNT(*) > %s",
        params=(asset_id, count_before_lock),
        timeout=20.0,
    )
    assert row is not None, (
        "no new position rows after lock release — writer did not drain"
    )

    # Server still responsive — still accepting, no hung worker join.
    assert server_proc["proc"].poll() is None, "server exited after DB stall"
