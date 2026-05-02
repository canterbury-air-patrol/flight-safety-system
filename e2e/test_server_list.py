"""The server should send a server_list message to newly-connected clients.

We assert by side-effect: the fake-client must keep its session healthy
(continuing to produce position rows) even when config_serverconfig has
multiple active entries."""
from __future__ import annotations

import time

import pytest


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
