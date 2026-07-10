"""e2e ADS-B forwarding to aircraft clients (TC-SRV-002 + Path C server share).

Covers todo/28: the server forwards position reports from non-aircraft
clients (e.g. an fss-adsb feeder) and from other aircraft to connected
aircraft clients, with no echo back to the reporting client. Rate limiting
in this codebase is a single per-connection token bucket (src/rate-
limiter.hpp), not per-ICAO as originally assumed when this todo was filed --
test 4 is scoped to what actually exists: N updates/sec from one feeder
connection are capped by that connection's own bucket.

MAVLink-side delivery (ADSB_VEHICLE into the autopilot) stays in Tier 3 Path
C/E -- out of scope here.
"""
from __future__ import annotations

import time

import pytest


def _log_text(client: dict) -> str:
    return client["log"].read_text(errors="replace")


def _wait_for_rcvd_pos(client: dict, icao_hex: str, timeout: float = 20.0) -> bool:
    needle = f"RCVD_POS: icao={icao_hex}"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if needle in _log_text(client):
            return True
        time.sleep(0.1)
    return False


@pytest.mark.satisfies("TC-SRV-002")
@pytest.mark.requires_docker
def test_adsb_forwarded_to_aircraft(db_conn, fake_client):
    """A non-aircraft client's position report for an arbitrary ICAO reaches
    a connected aircraft client."""
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")

    aircraft = fake_client("test1")
    # test3's cert is not enrolled as an asset -- required for non-aircraft
    # identify (the server rejects a non-aircraft CN that IS a known
    # aircraft).
    feeder = fake_client(
        "test3", non_aircraft=True,
        extra_args=["--icao=0xabc123", "--callsign=FEEDER", "--position-interval-ms=500"],
    )

    assert _wait_for_rcvd_pos(aircraft, "abc123"), (
        "aircraft client never received the feeder's position report\n" + _log_text(aircraft)
    )
    # The feeder itself must never see its own report relayed back (also
    # covered explicitly by test_no_echo below) or any aircraft position.
    assert "RCVD_POS" not in _log_text(feeder), (
        "non-aircraft feeder must never receive relayed position reports\n" + _log_text(feeder)
    )


@pytest.mark.requires_docker
def test_aircraft_to_aircraft_forwarding(db_conn, fake_client):
    """Positions from one aircraft are relayed to another (Path C c01)."""
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test2') RETURNING id")

    first = fake_client("test1", extra_args=["--icao=0x1111", "--position-interval-ms=1000"])
    second = fake_client("test2", extra_args=["--icao=0x2222", "--position-interval-ms=1000"])

    assert _wait_for_rcvd_pos(second, "1111"), (
        "second aircraft never received first aircraft's position\n" + _log_text(second)
    )
    assert _wait_for_rcvd_pos(first, "2222"), (
        "first aircraft never received second aircraft's position\n" + _log_text(first)
    )


@pytest.mark.requires_docker
def test_no_echo(db_conn, fake_client):
    """A client's own position reports are never relayed back to it (Path C
    c03) -- the sole connected client here has nothing else to receive."""
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")

    solo = fake_client("test1", extra_args=["--position-interval-ms=1000"])
    time.sleep(8)  # several send cycles

    assert "RCVD_POS" not in _log_text(solo), (
        "client must never receive its own relayed position report\n" + _log_text(solo)
    )


@pytest.mark.requires_docker
@pytest.mark.slow
def test_rate_limited_per_connection(db_conn, fake_client):
    """N updates/sec from one feeder connection are capped by that
    connection's per-connection token bucket (default capacity 100, refill
    20/s -- src/rate-limiter.hpp), not delivered 1:1 to the receiver."""
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")

    aircraft = fake_client("test1")
    fake_client(
        "test3", non_aircraft=True,
        extra_args=["--icao=0xfeed01", "--callsign=BURST", "--position-interval-ms=10"],
    )

    # ~2s at a 10ms interval is ~200 send attempts -- well over the default
    # bucket capacity (100) plus refill (20/s) for this window. The feeder
    # keeps running past this snapshot (stopped by the fixture's teardown);
    # reading the log now just takes a point-in-time count.
    time.sleep(2.5)

    received = _log_text(aircraft).count("RCVD_POS: icao=feed01")
    assert received > 0, "no bursted position reports were forwarded at all\n" + _log_text(aircraft)
    assert received < 150, (
        f"expected the per-connection rate limiter to cap delivery well under "
        f"the ~200 attempted sends, got {received}\n" + _log_text(aircraft)
    )
