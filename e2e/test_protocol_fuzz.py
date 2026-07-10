"""Malformed-frame robustness against the real server (TC-FSS-004 server share).

Covers todo/30: a deterministic, seeded corpus of hostile bytes thrown at the
real binary through the real socket/TLS stack -- a regression gate, not an
open-ended fuzz campaign. The unit layer already covers decode rejection
in isolation (malformed_message_test.cpp, oversized_message_test.cpp); this
file exercises the same shapes end-to-end and checks the server is still
alive and correct afterward.

Wire format (mirrors tests/test_helpers.hpp make_framed_buffer and the
packData layouts in src/transport-messages.cpp): a 12-byte header
(uint16 declared_length, uint16 type, uint64 id, all big-endian) followed by
the type-specific payload. message_type_position_report = 5,
message_type_identity = 2 (src/fss-transport.hpp's fss_message_type_e).
"""
from __future__ import annotations

import contextlib
import random
import socket
import ssl
import struct
import time
from pathlib import Path

import pytest

MSG_TYPE_IDENTITY = 2
MSG_TYPE_POSITION_REPORT = 5
HEADER_LEN = 12


def pack_frame(msg_type: int, msg_id: int, payload: bytes) -> bytes:
    declared_length = HEADER_LEN + len(payload)
    header = struct.pack(">HHQ", declared_length, msg_type, msg_id)
    return header + payload


def pack_identity_payload(name: str) -> bytes:
    """fss_message_identity::packData: raw name bytes, no length prefix."""
    return name.encode("ascii")


def _pack_string_field(buf: bytearray, s: bytes) -> None:
    """Mirrors packStringRaw: 2-byte BE length + bytes, then pad so the
    *whole* buffer built so far (header included) lands on an 8-byte
    boundary."""
    buf += struct.pack(">H", len(s))
    buf += s
    remainder = len(buf) % 8
    if remainder:
        buf += b"\x00" * (8 - remainder)


def pack_position_report_payload(
    *,
    timestamp: int,
    lat_raw: int,
    lng_raw: int,
    altitude: int,
    icao: int,
    heading: int,
    horz_vel: int,
    vert_vel: int,
    squawk: int,
    callsign: bytes,
    flags: int,
    alt_type: int,
    emitter_type: int,
    tslc: int,
) -> bytes:
    """Mirrors fss_message_position_report::packData. The callsign's packString
    padding is computed against the cumulative buffer length *including* the
    12-byte frame header, so build with a dummy header prefix and strip it."""
    buf = bytearray(HEADER_LEN)
    buf += struct.pack(">Q", timestamp & 0xFFFFFFFFFFFFFFFF)
    buf += struct.pack(">i", lat_raw)
    buf += struct.pack(">i", lng_raw)
    buf += struct.pack(">I", altitude & 0xFFFFFFFF)
    buf += struct.pack(">I", icao & 0xFFFFFFFF)
    buf += struct.pack(">H", heading & 0xFFFF)
    buf += struct.pack(">H", horz_vel & 0xFFFF)
    buf += struct.pack(">h", vert_vel)
    buf += struct.pack(">H", squawk & 0xFFFF)
    _pack_string_field(buf, callsign)
    buf += struct.pack(">H", flags & 0xFFFF)
    buf += struct.pack(">B", alt_type & 0xFF)
    buf += struct.pack(">B", emitter_type & 0xFF)
    buf += struct.pack(">B", tslc & 0xFF)
    return bytes(buf[HEADER_LEN:])


def _tls_connect(port: int, certs_dir: Path, name: str) -> ssl.SSLSocket:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.load_verify_locations(cafile=str(certs_dir / "ca.public.pem"))
    ctx.load_cert_chain(
        certfile=str(certs_dir / f"{name}.public.pem"),
        keyfile=str(certs_dir / f"{name}.private.pem"),
    )
    raw = socket.create_connection(("127.0.0.1", port), timeout=5.0)
    tls = ctx.wrap_socket(raw, server_hostname="localhost")
    tls.settimeout(5.0)
    return tls


def _assert_server_still_alive(server_proc: dict) -> None:
    assert server_proc["proc"].poll() is None, (
        "fss-server exited during protocol fuzzing:\n"
        + server_proc["log"].read_text(errors="replace")
    )


@pytest.mark.satisfies("TC-FSS-004")
@pytest.mark.requires_docker
def test_pre_tls_garbage_survives(server_proc, db_conn, fake_client):
    """Random bytes on a raw (pre-TLS) TCP connection must never crash or
    wedge the accept path; a real client can still connect afterward."""
    rng = random.Random(20260702)  # noqa: S311 - deterministic corpus, not security-sensitive
    for _ in range(30):
        with socket.create_connection(("127.0.0.1", server_proc["port"]), timeout=5.0) as s:
            s.settimeout(2.0)
            length = rng.randint(1, 256)
            s.sendall(bytes(rng.getrandbits(8) for _ in range(length)))
            with contextlib.suppress(OSError):
                s.recv(4096)

    _assert_server_still_alive(server_proc)

    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]
    client = fake_client("test1")
    time.sleep(7)
    with db_conn.cursor() as cur:
        cur.execute("SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s", (asset_id,))
        count = cur.fetchone()[0]
    assert count >= 1, (
        "no position rows from a real client after pre-TLS garbage phase\n"
        + client["log"].read_text(errors="replace")
    )


@pytest.mark.satisfies("TC-FSS-004")
@pytest.mark.requires_docker
def test_post_tls_malformed_frames_survive(server_proc, certs_dir, db_conn, reset_db, fake_client):
    """An authenticated peer sending malformed/absurd frames must not crash
    the server, corrupt other sessions, or produce a command row; normal
    traffic must keep working afterward."""
    with db_conn.cursor() as cur:
        cur.execute("INSERT INTO assets_asset (name) VALUES ('test1') RETURNING id")
        asset_id = cur.fetchone()[0]

    # Case 1: truncated header -- fewer than the 12 declared header bytes.
    with _tls_connect(server_proc["port"], certs_dir, "test1") as tls:
        tls.sendall(pack_frame(MSG_TYPE_IDENTITY, 1, pack_identity_payload("test1")))
        tls.sendall(struct.pack(">HH", 0xFFFF, MSG_TYPE_POSITION_REPORT)[:3])  # 3 of 12 header bytes

    # Case 2: oversized declared length with only a tiny payload following.
    with _tls_connect(server_proc["port"], certs_dir, "test1") as tls:
        tls.sendall(pack_frame(MSG_TYPE_IDENTITY, 1, pack_identity_payload("test1")))
        tls.sendall(struct.pack(">HHQ", 0xFFFF, MSG_TYPE_POSITION_REPORT, 2) + b"AAAA")

    # Case 3: unknown / undefined message type (15 is one past the last
    # real enumerator -- see malformed_message_test.cpp's todo/40 case).
    with _tls_connect(server_proc["port"], certs_dir, "test1") as tls:
        tls.sendall(pack_frame(MSG_TYPE_IDENTITY, 1, pack_identity_payload("test1")))
        tls.sendall(pack_frame(15, 2, b""))

    # Case 4: syntactically valid position_report, wildly out-of-range
    # values in every numeric field and an inconsistent flags bitmask.
    with _tls_connect(server_proc["port"], certs_dir, "test1") as tls:
        tls.sendall(pack_frame(MSG_TYPE_IDENTITY, 1, pack_identity_payload("test1")))
        absurd_payload = pack_position_report_payload(
            timestamp=2**63 - 1,
            lat_raw=2_000_000_000,   # decodes to a wildly out-of-range latitude
            lng_raw=-2_000_000_000,  # ditto longitude
            altitude=4_000_000_000,  # far above any real aircraft altitude
            icao=0xFFFFFFFF,
            heading=65535,           # real range is 0-3599 (tenths of a degree)
            horz_vel=65535,
            vert_vel=-32768,
            squawk=9999,             # squawk codes are octal 0-7777
            callsign=b"FUZZ" * 20,   # long but well-formed
            flags=0xFFFF,            # every flag bit set at once
            alt_type=255,
            emitter_type=255,
            tslc=255,
        )
        tls.sendall(pack_frame(MSG_TYPE_POSITION_REPORT, 2, absurd_payload))
        time.sleep(1.0)  # give the server a beat to process before we close

    _assert_server_still_alive(server_proc)

    # No fuzz input is a command, so no command row may exist.
    with db_conn.cursor() as cur:
        cur.execute("SELECT COUNT(*) FROM assets_assetcommand")
        command_rows = cur.fetchone()[0]
    assert command_rows == 0, "fuzz input must never produce a command row"

    # Liveness: a normal client still round-trips position + command traffic.
    with db_conn.cursor() as cur:
        cur.execute(
            "INSERT INTO assets_assetcommand (asset_id, command, position, altitude) "
            "VALUES (%s, 'GOTO', ST_SetSRID(ST_MakePoint(172.6, -43.6), 4326)::geography, 500)",
            (asset_id,),
        )
    client = fake_client("test1")
    time.sleep(7)
    with db_conn.cursor() as cur:
        cur.execute("SELECT COUNT(*) FROM assets_assetposition WHERE asset_id = %s", (asset_id,))
        pos_count = cur.fetchone()[0]
    assert pos_count >= 1, (
        "no position rows from a real client after the malformed-frame phase\n"
        + client["log"].read_text(errors="replace")
    )
    time.sleep(6)  # let the command poller pick up and deliver the injected command
    log_content = client["log"].read_text(errors="replace")
    assert "RCVD_CMD: GOTO" in log_content, (
        "command delivery broken after malformed-frame phase\n" + log_content
    )
