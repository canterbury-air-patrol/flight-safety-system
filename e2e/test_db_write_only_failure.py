"""e2e: a write-only DB failure latches the fail-safe until writes actually
work again (todo/45+47, todo/78).

The shipped todo/34 fail-safe severed every client on sustained DB write
failure but left the listener ungated. If the failure is write-only — reads
still succeed, so re-identify works — every severed aircraft reconnected
straight back into the same unhealthy server, failed writes again, and was
severed again ~db_write_failure_disconnect_secs later: a sustained
disconnect/reconnect flap oscillating aircraft in and out of comms-loss RTL.
(The pure disk-full case never showed this because Postgres PANICs and the
read side dies too; see test_db_disk_full.py.)

This test manufactures the write-only case with BEFORE INSERT triggers that
raise on the telemetry tables (the server connects as a superuser here, so
REVOKE would be bypassed; a trigger fires for any role). Reads are untouched,
which is exactly why a `SELECT 1`-style health check cannot be the fail-safe's
recovery evidence: it stays green throughout this test.

What is asserted: exactly one trip; the reconnecting client is refused (never
re-identified) while degraded; the degraded state HOLDS for as long as writes
keep failing, not merely until a quiet timer expires; and once the fault is
removed the fail-safe recovers on its own and the same client is readmitted
and stores telemetry again.

That middle property is todo/78, and it is what this file used to assert the
opposite of. Recovery was "the write queue drained and stayed quiet for
db_write_failure_recovery_grace_secs" — but the trip's own action (sever
everything, refuse everything) removes all the traffic that could produce a
failure, so drained-and-quiet was satisfied by construction ~15 s after every
trip, whatever the database was doing. Against a permanently dead database
that was a ~20 s flap forever (Path M m05 observed it live). The old version of
this test could only observe the no-flap window for 6 s, because at ~15 s the
server would have "recovered" into the still-broken database and the test would
have been asserting the defect. Recovery now additionally requires a successful
*probe write* — a real INSERT on the write connection, rolled back — which the
triggers here fail, so the hold below can be as long as we like.
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


# Worst-case budget for the waits below, so the next person does not have to
# re-derive it: 25 (baseline row) + 30 (trip) + 15 (first refusal) + 45 (the
# degraded hold) + 40 (recovery once the fault is removed) + 20 (re-identify)
# + 25 (telemetry resumes) = 200 s, plus this test's share of fixture setup
# (postgis container, migration, cert generation, server + client spawn), which
# pytest-timeout counts because it is not running in func_only mode. 300 s
# leaves ~100 s of headroom. The hold was 6 s before todo/78: recovery used to
# arrive on a ~15 s timer regardless of the database's state, so observing for
# longer would have caught the server "recovering" into a still-broken database
# — which is the defect, not the behaviour. e2e/pytest.ini's global 60 s default
# is overridden by this decorator.
@pytest.mark.requires_docker
@pytest.mark.slow
@pytest.mark.timeout(300)
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

        # The incident must span db_write_failure_disconnect_secs (5s) of
        # recurring failures before tripping; RTT writes recur ~1s.
        assert _wait_for(lambda: "DB fail-safe tripped" in log_text(), timeout=30.0, poll=0.2), (
            "server never tripped the fail-safe on sustained write-only failure\n" + log_text()
        )

        # The severed client's automatic reconnect attempts must be refused at
        # admission while degraded.
        assert _wait_for(lambda: "Refusing new client" in log_text(), timeout=15.0, poll=0.2), (
            "no reconnect attempt was refused while degraded\n" + log_text()
        )

        # No-flap window, and the todo/78 property: the degraded state must
        # hold for as long as writes are broken. Three times the 15s recovery
        # grace, checked continuously rather than only at the end — a recovery
        # that happened and re-tripped would otherwise be invisible in the final
        # snapshot. Pre-todo/78 this reached the first assert inside ~16s.
        hold_secs = 45
        deadline = time.monotonic() + hold_secs
        while time.monotonic() < deadline:
            snapshot = log_text()
            assert "DB fail-safe recovered" not in snapshot, (
                "fail-safe recovered while every write to the database was still failing "
                f"({int(hold_secs - (deadline - time.monotonic()))}s into a {hold_secs}s hold)\n" + snapshot
            )
            assert snapshot.count("DB fail-safe tripped") == 1, (
                "fail-safe tripped more than once while degraded (flap)\n" + snapshot
            )
            time.sleep(0.5)

        during_degraded = log_text()
        assert during_degraded.count("Aircraft client identified: test1") == 1, (
            "client was re-admitted to identified operation while degraded\n" + during_degraded
        )
        # The probe is what holds the latch shut, so it must be visibly running:
        # a server that simply stopped probing would pass the asserts above for
        # the wrong reason.
        assert "fail-safe probe write failed" in during_degraded, (
            "no failing probe write was logged while degraded — the fail-safe is holding on "
            "silence rather than on evidence (todo/78)\n" + during_degraded
        )
    finally:
        _remove_write_failure(db_conn)

    # With the fault removed the probe succeeds within a cadence or two, and
    # recovery follows once the grace window (15s from the last failure, which
    # is the last failing probe) has also elapsed.
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
