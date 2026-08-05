"""e2e: a database that keeps failing holds the gate longer each time (todo/81).

todo/78 closed the *permanent* failure case: recovery needs a probe write to
have succeeded, and against a dead database it never does, so the fail-safe
holds and the aircraft gets one latched comms-loss event
(test_db_write_only_failure.py asserts exactly that).

This file drives the case todo/78 left open. The fault here is *intermittent*:
it is installed, removed once the fail-safe has tripped, and installed again
once the readmitted fleet is writing. Every readmission is therefore backed by
honest evidence — the queue really drained, the window really went quiet, a
probe write really succeeded — and the aircraft still cycles in and out of its
comms-loss state as the fault returns. The recovery grace now backs off per
re-trip, so the second episode holds the gate for twice the configured window,
which is what this test observes.

The fault is driven from the test rather than from a self-toggling trigger
(a counter table, or random() in the trigger function) because the behaviour
under test is about the *repetition*: the point that has to be pinned is that
episode two costs more than episode one, and that is only legible if the test
decides when each episode begins and ends.

The observable is the recovery log line, which names the window the episode
actually cleared: "quiet for 15s" the first time and "quiet for 30s" the
second. The wall-clock hold is asserted too — the log line and the behaviour
could in principle disagree, and the one that matters to an aircraft is the
hold.
"""
from __future__ import annotations

import time

import pytest

from conftest import _wait_for
from test_db_write_only_failure import _install_write_failure, _remove_write_failure

# The server's defaults, which this test deliberately runs at rather than
# tuning down: they are what a deployment gets, and the multiplier is only
# meaningful against the configured grace.
RECOVERY_GRACE_SECS = 15
DISCONNECT_SECS = 5


def _count(text: str, needle: str) -> int:
    return text.count(needle)


# Worst-case budget, so the next person does not have to re-derive it:
# 25 (baseline row) + 30 (first trip) + 40 (first recovery) + 20 (re-identify)
# + 25 (telemetry resumes) + 40 (second trip) + 70 (the backed-off second
# recovery: 30 s of quiet, plus the probe cadence and the slack the first
# recovery gets) = 250 s, plus this test's share of fixture setup (postgis
# container, migration, cert generation, server + client spawn), which
# pytest-timeout counts because it is not running in func_only mode. The second
# recovery is the one wait that cannot be shortened without disabling the very
# behaviour under test. e2e/pytest.ini's global 60 s default is overridden here.
@pytest.mark.requires_docker
@pytest.mark.slow
@pytest.mark.timeout(420)
def test_repeated_write_failure_holds_the_gate_longer(db_conn, fake_client, server_proc):
    """An intermittent write fault: the second degraded episode holds the
    admission gate for twice as long as the first."""
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

    def run_episode(number: int, trip_timeout: float, recovery_timeout: float) -> float:
        """Install the fault, wait for the trip, remove it, wait for recovery.

        Returns how long the gate stayed up after the fault was removed — the
        number an aircraft experiences, measured the same way for both
        episodes so they can be compared.
        """
        try:
            _install_write_failure(db_conn)
            # The incident must span db_write_failure_disconnect_secs of
            # recurring failures before tripping; RTT writes recur ~1s.
            assert _wait_for(
                lambda: _count(log_text(), "DB fail-safe tripped") == number,
                timeout=trip_timeout,
                poll=0.2,
            ), (
                f"fail-safe did not trip for episode {number} "
                f"(disconnect threshold {DISCONNECT_SECS}s)\n" + log_text()
            )
        finally:
            _remove_write_failure(db_conn)
        # From here the database is healthy: the probe starts succeeding within
        # a cadence or two and only the quiet window stands between the fleet
        # and readmission. That is what makes this an honest measurement of the
        # window rather than of how long the fault happened to last.
        healed_at = time.monotonic()
        assert _wait_for(
            lambda: _count(log_text(), "DB fail-safe recovered") == number,
            timeout=recovery_timeout,
            poll=0.5,
        ), (
            f"fail-safe never recovered for episode {number} after the write fault was "
            "removed\n" + log_text()
        )
        return time.monotonic() - healed_at

    first_hold = run_episode(1, trip_timeout=30.0, recovery_timeout=40.0)

    # The gate is down: the same client re-identifies and telemetry stores
    # again. The second episode needs the fleet actually writing, or there is
    # nothing to fail.
    assert _wait_for(
        lambda: log_text().count("Aircraft client identified: test1") >= 2, timeout=20.0
    ), "client never re-identified after the first recovery\n" + log_text()
    resumed_from = position_count()
    assert _wait_for(lambda: position_count() > resumed_from, timeout=25.0), (
        "telemetry did not resume storing after the first recovery\n"
        f"-- server log --\n{log_text()}\n"
        f"-- client log --\n{client['log'].read_text(errors='replace')}"
    )

    # Same fault, same server, within the hour: this is a re-trip, not a fresh
    # incident, and the recovery grace doubles.
    second_hold = run_episode(2, trip_timeout=40.0, recovery_timeout=70.0)

    final = log_text()
    # The log names the window each episode actually cleared. An operator
    # watching a flapping database reads this line to know when the fleet comes
    # back, so it has to report the effective window and not the configured one.
    assert f"quiet for {RECOVERY_GRACE_SECS}s" in final, (
        "the first recovery did not report the configured grace window\n" + final
    )
    assert f"quiet for {2 * RECOVERY_GRACE_SECS}s" in final, (
        "the second recovery did not report a backed-off grace window — the re-trip was "
        "treated as a fresh incident (todo/81)\n" + final
    )

    # And the behaviour itself, not just the line describing it. The second
    # hold must clear the un-backed-off window by a margin no scheduling slop
    # accounts for; both holds are measured from the moment the database became
    # healthy again, so the difference is the back-off and nothing else.
    assert second_hold > RECOVERY_GRACE_SECS + 5, (
        f"the second degraded episode held the gate for {second_hold:.1f}s, no longer than "
        f"the configured {RECOVERY_GRACE_SECS}s grace — the fleet is being readmitted into "
        "a database that has already failed twice\n" + final
    )
    assert second_hold > first_hold + 5, (
        f"the second degraded episode ({second_hold:.1f}s) did not hold materially longer "
        f"than the first ({first_hold:.1f}s)\n" + final
    )

    # Two episodes, two trips, two recoveries: the back-off lengthens the hold,
    # it does not add or suppress transitions.
    assert _count(final, "DB fail-safe tripped") == 2
    assert _count(final, "DB fail-safe recovered") == 2
    assert client["proc"].poll() is None, "fake client died during the test"
    assert server_proc["proc"].poll() is None, "server died during the test"
