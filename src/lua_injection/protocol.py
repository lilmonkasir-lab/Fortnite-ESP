"""Line-delimited JSON protocol shared by the controller and test host."""

from __future__ import annotations

import json
import socket
from typing import Any, BinaryIO

PROTOCOL_VERSION = 1
MAX_MESSAGE_BYTES = 512_000


class ProtocolError(Exception):
    """The peer sent an invalid or oversized protocol message."""


def encode_message(message: dict[str, Any]) -> bytes:
    if not isinstance(message, dict):
        raise ProtocolError("protocol messages must be objects")
    try:
        payload = json.dumps(message, separators=(",", ":"), ensure_ascii=False).encode("utf-8") + b"\n"
    except (TypeError, ValueError) as exc:
        raise ProtocolError(f"message is not JSON-compatible: {exc}") from exc
    if len(payload) > MAX_MESSAGE_BYTES:
        raise ProtocolError(f"message exceeds {MAX_MESSAGE_BYTES:,} bytes")
    return payload


def decode_message(line: bytes) -> dict[str, Any]:
    if len(line) > MAX_MESSAGE_BYTES:
        raise ProtocolError(f"message exceeds {MAX_MESSAGE_BYTES:,} bytes")
    try:
        message = json.loads(line.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProtocolError(f"invalid JSON message: {exc}") from exc
    if not isinstance(message, dict):
        raise ProtocolError("protocol messages must be JSON objects")
    return message


def read_message(stream: BinaryIO) -> dict[str, Any] | None:
    line = stream.readline(MAX_MESSAGE_BYTES + 1)
    if not line:
        return None
    if len(line) > MAX_MESSAGE_BYTES:
        raise ProtocolError(f"message exceeds {MAX_MESSAGE_BYTES:,} bytes")
    return decode_message(line)


def write_message(stream: BinaryIO, message: dict[str, Any]) -> None:
    stream.write(encode_message(message))
    stream.flush()


def request_id(message: dict[str, Any]) -> str:
    value = message.get("id")
    if not isinstance(value, str) or not value or len(value) > 100:
        raise ProtocolError("request id must be a non-empty string up to 100 characters")
    return value


def get_message_type(message: dict[str, Any]) -> str:
    value = message.get("type")
    if not isinstance(value, str) or not value:
        raise ProtocolError("message type must be a non-empty string")
    return value


def close_socket_quietly(sock: socket.socket | None) -> None:
    if sock is None:
        return
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass


__all__ = [
    "MAX_MESSAGE_BYTES",
    "PROTOCOL_VERSION",
    "ProtocolError",
    "close_socket_quietly",
    "decode_message",
    "encode_message",
    "get_message_type",
    "read_message",
    "request_id",
    "write_message",
]
