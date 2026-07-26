"""The server should send a server_list message to newly-connected clients.

We assert by side-effect: the fake-client must keep its session healthy
(continuing to produce position rows) even when config_serverconfig has
multiple active entries.

The suite covered only the *learning* direction until todo/67 added a removal
path to the client; test_deactivated_server_is_dropped_by_client covers losing
one."""
from __future__ import annotations

import time

import pytest


@pytest.mark.satisfies("TC-SRV-003")
@pytest.mark.requires_docker
def test_server_list_does_not_disrupt_session(db_conn, fake_client):
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]
        cur.execute(
            "INSERT INTO config_serverconfig (address, client_port, active) "
            "VALUES ('localhost', 20202, TRUE), ('backup.example', 20203, TRUE)"
        )

    client = fake_client("test1")

    # Server checks server list on client connection; if server_list handling
    # is broken this is where we'd see the client drop.
    time.sleep(12)

    assert client["proc"].poll() is None, (
        "fake-client exited while processing server_list:\n"
        + client["log"].read_text(errors="replace")
    )

    with db_conn.cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s",
            (asset_id,),
        )
        count = cur.fetchone()[0]
    assert count >= 1, "no positions after server_list delivery"


@pytest.mark.satisfies("TC-SRV-003")
@pytest.mark.requires_docker
def test_server_list_with_full_serverconfig_columns(db_conn, fake_client):
    """Rows with name/config_port/https populated must not break server_list delivery.

    Regression for TEST-02: e2e schema previously omitted these three Django
    columns; any INSERT that included them would fail at schema creation time.
    """
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]
        cur.execute(
            "INSERT INTO config_serverconfig "
            "  (address, client_port, active, name, config_port, https) "
            "VALUES ('primary.example', 20202, TRUE, 'primary', 8090, FALSE)"
        )

    client = fake_client("test1")

    time.sleep(12)

    assert client["proc"].poll() is None, (
        "fake-client exited after server_list with full-column row:\n"
        + client["log"].read_text(errors="replace")
    )

    with db_conn.cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s",
            (asset_id,),
        )
        count = cur.fetchone()[0]
    assert count >= 1, "no positions recorded after server_list with full-column row"


@pytest.mark.satisfies("TC-SRV-003")
@pytest.mark.requires_docker
def test_deactivated_server_is_dropped_by_client(db_conn, fake_client):
    """A server removed from config_serverconfig must leave the client's list.

    todo/67: updateServers() only ever added, so a decommissioned server stayed
    in every client that had ever seen it for the process lifetime, costing a
    blocking connect attempt per backoff interval -- and silently, because
    nothing logged it. The client now drops a learned server that has been
    absent from every list for the whole expiry window, and says so.

    The window is lowered from its 60 s default so this does not have to sit
    through four broadcast rounds; the server rebuilds and rebroadcasts its
    cached list every 15 s, so the drop lands within roughly two rounds of the
    row being deactivated.
    """
    expiry_ms = 10000
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]
        cur.execute(
            "INSERT INTO config_serverconfig (address, client_port, active) "
            "VALUES ('doomed.example', 20303, TRUE) RETURNING id"
        )
        doomed_id = cur.fetchone()[0]

    client = fake_client("test1", learned_expiry_ms=expiry_ms)

    # One broadcast round is enough for the client to learn it. It surfaces in
    # the log because the client then tries to dial it and the address does not
    # resolve ("Failed to convert 'doomed.example' to a usable address") --
    # which is itself the cost todo/67 is about paying forever.
    deadline = time.monotonic() + 30
    learned = False
    while time.monotonic() < deadline:
        if "doomed.example" in client["log"].read_text(errors="replace"):
            learned = True
            break
        time.sleep(1)
    assert learned, (
        "client never mentioned the advertised server:\n"
        + client["log"].read_text(errors="replace")
    )

    with db_conn.cursor() as cur:
        cur.execute("UPDATE config_serverconfig SET active = FALSE WHERE id = %s", (doomed_id,))

    # Worst case: up to 15 s for the server to rebuild its cached list without
    # the row, then the expiry window, then the next broadcast to notice.
    deadline = time.monotonic() + 60
    dropped = False
    while time.monotonic() < deadline:
        log = client["log"].read_text(errors="replace")
        if "doomed.example:20303" in log and "dropping it" in log:
            dropped = True
            break
        time.sleep(1)

    log = client["log"].read_text(errors="replace")
    assert dropped, f"client never dropped the deactivated server:\n{log}"
    assert client["proc"].poll() is None, f"fake-client exited during expiry:\n{log}"

    # Dropping a server must not disturb the session with the live one.
    with db_conn.cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s",
            (asset_id,),
        )
        count = cur.fetchone()[0]
    assert count >= 1, "no positions recorded while the stale server was expiring"
