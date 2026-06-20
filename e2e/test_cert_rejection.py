"""A client presenting a cert signed by a foreign CA must not be able to persist data."""
from __future__ import annotations

import shutil
import subprocess
import time

import pytest


@pytest.fixture(scope="session")
def alt_certs_dir(tmp_path_factory, certs_dir):
    """A totally separate CA + client cert. The server is configured to
    trust only the main CA, so a client using alt_certs will fail handshake."""
    from conftest import CERT_SCRIPTS
    target = tmp_path_factory.mktemp("certs-alt")
    for name in ("generate-ca.sh", "generate-server.sh", "generate-client.sh",
                 "ca.tmpl", "client.tmpl", "localhost.tmpl"):
        src = CERT_SCRIPTS / name
        if src.exists():
            shutil.copy(src, target / name)
    for script in ("generate-ca.sh", "generate-server.sh", "generate-client.sh"):
        (target / script).chmod(0o755)
    subprocess.check_call(["./generate-ca.sh"], cwd=str(target))
    subprocess.check_call(["./generate-client.sh", "test1"], cwd=str(target))
    return target


@pytest.mark.requires_docker
def test_client_with_wrong_ca_rejected(db_conn, fake_client, alt_certs_dir):
    """The session brought up by server_proc trusts the main CA. An
    alt-CA-signed client should fail to authenticate and persist zero rows."""
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]

    fake_client("test1", ca_override=alt_certs_dir)

    # Give the rejected client time to fail and retry a few times.
    time.sleep(12)

    with db_conn.cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s",
            (asset_id,),
        )
        count = cur.fetchone()[0]
    assert count == 0, (
        f"expected 0 position rows for rejected client, got {count}"
    )
