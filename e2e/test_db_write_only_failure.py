"""e2e: a write-only DB failure latches one fail-safe event, no flap (todo/45+47).

The shipped todo/34 fail-safe severed every client on sustained DB write
failure but left the listener ungated. If the failure is write-only — reads
still succeed, so re-identify works — every severed aircraft reconnected
straight back into the same unhealthy server, failed writes again, and was
severed again ~db_write_failure_disconnect_ticks later: a sustained
disconnect/reconnect flap oscillating aircraft in and out of comms-loss RTL.
(The pure disk-full case never showed this because Postgres PANICs and the
read side dies too; see test_db_disk_full.py.)

This test manufactures the write-only case with BEFORE INSERT triggers that
raise on the telemetry tables (the server connects as a superuser here, so
REVOKE would be bypassed; a trigger fires for any role). Reads are untouched.
It asserts the todo/47 behaviour: exactly one trip, the reconnecting client
is refused (never re-identified) while degraded, and once the fault is
removed the fail-safe recovers on its own — the write queue drains and stays
quiet for db_write_failure_recovery_grace_secs (15s default) — after which
the same client is readmitted and telemetry stores again.

Timing note: while degraded no writes are attempted (all clients are severed
and refused), so the quiet window elapses ~15s after the trip even with the
triggers still installed — recovery deliberately readmits traffic as the
health probe. The no-flap observation window must therefore stay well inside
those 15s, and the triggers are dropped immediately after it so the probe
succeeds; a persistent fault would simply re-latch on the incident timescale.
"""
from __future__ import annotations

import time

import psycopg2
import pytest

from conftest import _wait_for

TELEMETRY_TABLES = (
    "assets_assetposition",
    "assets_assetrtt",
    "assets_assetstatus",
    "assets_assetsearchprogress",
)


def _install_write_failure(db_conn: psycopg2.extensions.connection) -> None:
    with db_conn.cursor() as cur:
        cur.execute(
            "CREATE OR REPLACE FUNCTION e2e_fail_write() RETURNS trigger AS $$\n"
            "BEGIN\n"
            "    RAISE EXCEPTION 'e2e simulated write-only failure';\n"
            "END;\n"
            "$$ LANGUAGE plpgsql"
        )
        for table in TELEMETRY_TABLES:
            # sourcery skip: sqlalchemy-execute-raw-query — table names from
            # the constant tuple above, not external input
            cur.execute(
                f"CREATE TRIGGER e2e_fail_write_{table} BEFORE INSERT ON {table} "  # noqa: S608
                "FOR EACH ROW EXECUTE FUNCTION e2e_fail_write()"
            )


def _remove_write_failure(db_conn: psycopg2.extensions.connection) -> None:
    with db_conn.cursor() as cur:
        for table in TELEMETRY_TABLES:
            # sourcery skip: sqlalchemy-execute-raw-query
            cur.execute(f"DROP TRIGGER IF EXISTS e2e_fail_write_{table} ON {table}")  # noqa: S608
        cur.execute("DROP FUNCTION IF EXISTS e2e_fail_write()")


@pytest.mark.requires_docker
@pytest.mark.slow
@pytest.mark.timeout(180)
def test_write_only_db_failure_latches_one_failsafe_event(db_conn, fake_client, server_proc):
    """Write-only DB failure: one latched severance while degraded, refusal of
    reconnect attempts, autonomous recovery once writes heal, no flap."""
    log_path = server_proc["log"]

    def log_text() -> str:
        return log_path.read_text(errors="replace")

    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]

    client = fake_client("test1")

    def position_count() -> int:
        with db_conn.cursor() as cur:
            cur.execute(
                "SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s", (asset_id,)
            )
            return cur.fetchone()[0]

    assert _wait_for(lambda: position_count() > 0, timeout=25.0), (
        "no baseline position row before injecting the write failure\n"
        f"-- server log --\n{log_text()}\n"
        f"-- client log --\n{client['log'].read_text(errors='replace')}"
    )

    try:
        _install_write_failure(db_conn)

        # Reads keep working while writes fail — the property that makes this
        # failure mode able to flap at all (re-identify only needs a read).
        with db_conn.cursor() as cur:
            cur.execute("SELECT id FROM assets_asset WHERE name = 'test1'")
            assert cur.fetchone()[0] == asset_id

        # The incident must span db_write_failure_disconnect_ticks (5s) of
        # recurring failures before tripping; RTT writes recur ~1s.
        assert _wait_for(lambda: "DB fail-safe tripped" in log_text(), timeout=30.0, poll=0.2), (
            "server never tripped the fail-safe on sustained write-only failure\n" + log_text()
        )

        # The severed client's automatic reconnect attempts must be refused at
        # admission while degraded.
        assert _wait_for(lambda: "Refusing new client" in log_text(), timeout=15.0, poll=0.2), (
            "no reconnect attempt was refused while degraded\n" + log_text()
        )

        # No-flap window. Pre-fix, the client re-identified within ~1-2s of
        # the severance and was severed again ~5s later; 6s of observation
        # catches that cycle while staying well inside the ~15s quiet window
        # after which the (deliberate) autonomous recovery would readmit —
        # see the module docstring.
        time.sleep(6)
        during_degraded = log_text()
        assert during_degraded.count("DB fail-safe tripped") == 1, (
            "fail-safe tripped more than once while degraded (flap)\n" + during_degraded
        )
        assert during_degraded.count("Aircraft client identified: test1") == 1, (
            "client was re-admitted to identified operation while degraded\n" + during_degraded
        )
        assert "DB fail-safe recovered" not in during_degraded, (
            "fail-safe recovered while writes were still failing\n" + during_degraded
        )
    finally:
        _remove_write_failure(db_conn)

    # With the fault removed, the queue is drained and quiet: recovery follows
    # once the grace window (15s from the last failure) elapses.
    assert _wait_for(lambda: "DB fail-safe recovered" in log_text(), timeout=40.0, poll=0.5), (
        "fail-safe never recovered after the write fault was removed\n" + log_text()
    )

    # The gate is down: the same client re-identifies and telemetry stores
    # again against the same asset row.
    assert _wait_for(
        lambda: log_text().count("Aircraft client identified: test1") >= 2, timeout=20.0
    ), "client never re-identified after recovery\n" + log_text()

    resumed_from = position_count()
    assert _wait_for(lambda: position_count() > resumed_from, timeout=25.0), (
        "telemetry did not resume storing after recovery\n"
        f"-- server log --\n{log_text()}\n"
        f"-- client log --\n{client['log'].read_text(errors='replace')}"
    )

    # Whole-run invariant: one incident, one latched severance.
    assert log_text().count("DB fail-safe tripped") == 1
    assert client["proc"].poll() is None, "fake client died during the test"
    assert server_proc["proc"].poll() is None, "server died during the test"
