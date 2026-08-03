#!/usr/bin/env python3
"""Exercise the local opera-test-harness presenter protocol without an emulator."""

from __future__ import annotations

import socket
import struct
import sys
import time
from pathlib import Path


MAGIC = 0x5652504F
VERSION = 2
HELLO = 1
ACK = 2
START = 3
VIDEO = 4
AUDIO = 5
END = 6
DISCONNECT_TIMEOUT_SECONDS = 3.0


def message(kind: int, payload: bytes = b"") -> bytes:
    return struct.pack("<IIII", MAGIC, VERSION, kind, len(payload)) + payload


def receive_exact(connection: socket.socket, size: int) -> bytes:
    result = bytearray()
    while len(result) < size:
        part = connection.recv(size - len(result))
        if not part:
            raise RuntimeError("presenter closed the connection")
        result.extend(part)
    return bytes(result)


def expect_ack(connection: socket.socket) -> None:
    header = receive_exact(connection, 16)
    magic, version, kind, size = struct.unpack("<IIII", header)
    if (magic, version, kind, size) != (MAGIC, VERSION, ACK, 4):
        raise RuntimeError(f"invalid ACK header: {(magic, version, kind, size)!r}")
    (status,) = struct.unpack("<I", receive_exact(connection, 4))
    if status != 0:
        raise RuntimeError(f"presenter rejected message with status {status}")


def expect_stalled_disconnect(path: Path, partial_message: bytes) -> None:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(DISCONNECT_TIMEOUT_SECONDS)
        connection.connect(str(path))
        connection.sendall(partial_message)
        try:
            received = connection.recv(1)
        except TimeoutError as error:
            raise RuntimeError("presenter did not disconnect stalled client") from error
        if received:
            raise RuntimeError("presenter sent data before receiving a complete message")


def expect_trickle_disconnect(path: Path, complete_message: bytes) -> None:
    started = time.monotonic()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(DISCONNECT_TIMEOUT_SECONDS)
        connection.connect(str(path))
        for byte in complete_message:
            try:
                connection.sendall(bytes((byte,)))
            except (BrokenPipeError, ConnectionResetError):
                break
            time.sleep(0.3)
        else:
            received = connection.recv(1)
            if received:
                raise RuntimeError("presenter accepted a message beyond its deadline")
    if (time.monotonic() - started) >= DISCONNECT_TIMEOUT_SECONDS:
        raise RuntimeError("slow trickle kept the presenter client alive")


def run_once(path: Path, label: str, sample_rate: int) -> None:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(2.0)
        connection.connect(str(path))
        connection.sendall(message(HELLO, label.encode("utf-8")))
        expect_ack(connection)
        connection.sendall(
            message(START, struct.pack("<III", sample_rate, 60_000, 1_333_333))
        )
        expect_ack(connection)
        pixels = bytes((255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255))
        connection.sendall(message(VIDEO, struct.pack("<QII", 1, 2, 2) + pixels))
        samples = struct.pack("<hhhh", 1000, -1000, 500, -500)
        connection.sendall(message(AUDIO, struct.pack("<I", 2) + samples))
        connection.sendall(message(END, struct.pack("<I", 0)))


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: presenter_protocol_client.py SOCKET", file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    hello = message(HELLO, b"Stalled client")
    expect_stalled_disconnect(path, hello[:8])
    expect_stalled_disconnect(path, hello[:-1])
    expect_trickle_disconnect(path, hello)
    # Exercise device reuse first, then conversion-stream reconfiguration.
    run_once(path, "Protocol smoke one", 44_100)
    run_once(path, "Protocol smoke two", 44_100)
    run_once(path, "Protocol smoke three", 48_000)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
