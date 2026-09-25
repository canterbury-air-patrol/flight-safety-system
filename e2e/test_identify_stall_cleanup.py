"""A timed-out identification must not stop heartbeats for admitted aircraft."""
from __future__ import annotations

import time

import psycopg2
import pytest

from conftest import _wait_for, wait_for_row


@pytest.mark.requires_docker
@pytest.mark.slow
@pytest.mark.timeout(100)
def test_stalled_identification_cleanup_keeps_main_loop_running(
    db_conn, migrated_db, fake_client, server_proc,
):
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1'), ('test2')")
    healthy = fake_client("test1")
    assert wait_for_row(
        db_conn, "SELECT id FROM assets_assetposition LIMIT 1", timeout=15,
    ) is not None
    healthy_log = healthy["log"]
    assert _wait_for(lambda: "RCVD_RTT_REQ" in healthy_log.read_text(), timeout=10)

    # Existing sessions can answer heartbeats without this table, but the new
    # identity's registry lookup (or the read mutex ahead of it) cannot finish.
    lock_conn = psycopg2.connect(
        host=migrated_db["host"], port=migrated_db["port"],
        user=migrated_db["user"], password=migrated_db["password"],
        dbname=migrated_db["dbname"],
    )
    try:
        with lock_conn.cursor() as cur:
            cur.execute("LOCK TABLE assets_asset IN ACCESS EXCLUSIVE MODE")
        fake_client("test2")
        assert _wait_for(
            lambda: "Client did not identify within deadline" in server_proc["log"].read_text(),
            timeout=40,
        ), "the blocked session never reached its identification deadline"
        # Give the next main-loop cleanup sweep time to run. Before the fix it
        # joins test2's receive thread and cannot send another heartbeat.
        time.sleep(2)
        before = healthy_log.read_text().count("RCVD_RTT_REQ")
        assert _wait_for(
            lambda: healthy_log.read_text().count("RCVD_RTT_REQ") >= before + 3,
            timeout=6,
        ), "cleanup of a DB-blocked identity stopped fleet heartbeats"
    finally:
        lock_conn.rollback()
        lock_conn.close()

    # The removed callback cannot reclaim its asset when the DB unblocks;
    # its replacement must be admitted normally, with no stale identity claim.
    assert _wait_for(
        lambda: "Aircraft client identified: test2" in server_proc["log"].read_text(),
        timeout=20,
    )
