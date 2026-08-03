"""Controller-side client for the Lua Injection host protocol."""

from __future__ import annotations

import socket
import time
import uuid
from pathlib import Path
from typing import Any

from .host import HostDescriptor
from .protocol import (
    PROTOCOL_VERSION,
    ProtocolError,
    close_socket_quietly,
    read_message,
    write_message,
)


class AttachError(Exception):
    """The controller could not establish or use a host session."""


class LuaInjectionClient:
    """A small, synchronous client with explicit attach/detach lifecycle."""

    def __init__(self, descriptor: HostDescriptor, *, connect_timeout: float = 5.0):
        self.descriptor = descriptor
        self.connect_timeout = connect_timeout
        self.socket: socket.socket | None = None
        self.reader: Any = None
        self.writer: Any = None
        self.hello: dict[str, Any] | None = None
        self.authenticated = False

    @property
    def attached(self) -> bool:
        return self.socket is not None and self.authenticated

    def connect(self) -> dict[str, Any]:
        if self.attached:
            return self.hello or {}
        try:
            self.socket = socket.create_connection(
                (self.descriptor.host, self.descriptor.port), timeout=self.connect_timeout
            )
            self.socket.settimeout(self.connect_timeout)
            self.reader = self.socket.makefile("rb")
            self.writer = self.socket.makefile("wb")
            hello = read_message(self.reader)
            if hello is None or hello.get("type") != "hello":
                raise AttachError("host closed before sending hello")
            if hello.get("protocol") != PROTOCOL_VERSION:
                raise AttachError(
                    f"protocol mismatch: host={hello.get('protocol')}, client={PROTOCOL_VERSION}"
                )
            if self.descriptor.host_id and hello.get("host_id") != self.descriptor.host_id:
                raise AttachError("host identity does not match the descriptor")
            self.hello = hello
            write_message(self.writer, {"type": "auth", "token": self.descriptor.token})
            response = read_message(self.reader)
            if response is None or response.get("type") != "auth_ok":
                error = response.get("error", "authentication failed") if response else "host closed"
                raise AttachError(str(error))
            self.authenticated = True
            return hello
        except (OSError, ProtocolError) as exc:
            self.close()
            raise AttachError(f"could not attach to {self.descriptor.host}:{self.descriptor.port}: {exc}") from exc
        except AttachError:
            self.close()
            raise

    def connect_with_retry(self, retries: int = 3, delay: float = 0.25) -> dict[str, Any]:
        if retries < 1:
            raise ValueError("retries must be at least 1")
        last_error: AttachError | None = None
        for attempt in range(retries):
            try:
                return self.connect()
            except AttachError as exc:
                last_error = exc
                if attempt + 1 < retries:
                    time.sleep(delay)
        assert last_error is not None
        raise last_error

    def request(self, message_type: str, **payload: Any) -> Any:
        if not self.attached:
            self.connect()
        assert self.reader is not None and self.writer is not None
        identifier = uuid.uuid4().hex
        message = {"type": message_type, "id": identifier, **payload}
        try:
            write_message(self.writer, message)
            response = read_message(self.reader)
        except (OSError, ProtocolError) as exc:
            self.close()
            raise AttachError(f"host session lost: {exc}") from exc
        if response is None:
            self.close()
            raise AttachError("host closed the session")
        if response.get("type") != "response" or response.get("id") != identifier:
            raise AttachError("received an unexpected host response")
        if not response.get("ok"):
            raise AttachError(str(response.get("error", "host rejected the request")))
        return response.get("result")

    def ping(self) -> dict[str, Any]:
        return self.request("ping")

    def get_state(self) -> dict[str, Any]:
        return self.request("get_state")

    def reset_state(self) -> dict[str, Any]:
        return self.request("reset_state")

    def validate(
        self, script: str, *, timeout_ms: int = 2_000, instruction_limit: int = 100_000
    ) -> dict[str, Any]:
        return self.request(
            "validate",
            script=script,
            timeout_ms=timeout_ms,
            instruction_limit=instruction_limit,
        )

    def execute(
        self,
        script: str,
        *,
        timeout_ms: int = 2_000,
        instruction_limit: int = 100_000,
    ) -> dict[str, Any]:
        return self.request(
            "execute",
            script=script,
            timeout_ms=timeout_ms,
            instruction_limit=instruction_limit,
        )

    def detach(self) -> None:
        if self.attached:
            try:
                self.request("detach")
            except AttachError:
                # Detach is best-effort; the host may have exited already.
                pass
        self.close()

    def close(self) -> None:
        self.authenticated = False
        for stream in (self.reader, self.writer):
            if stream is not None:
                try:
                    stream.close()
                except OSError:
                    pass
        self.reader = None
        self.writer = None
        close_socket_quietly(self.socket)
        self.socket = None

    def __enter__(self) -> LuaInjectionClient:
        self.connect()
        return self

    def __exit__(self, *_: Any) -> None:
        self.detach()


def descriptor_from_path(path: str | Path) -> HostDescriptor:
    return HostDescriptor.read(path)


__all__ = ["AttachError", "LuaInjectionClient", "descriptor_from_path"]
