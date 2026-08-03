"""Loopback-only, token-authenticated test host for Lua Injection.

The host is intentionally opt-in: it is a small server started by the test
application itself.  It does not inspect processes, load DLLs, patch memory, or
attach to third-party applications.
"""

from __future__ import annotations

import hmac
import secrets
import socket
import socketserver
import threading
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .protocol import (
    PROTOCOL_VERSION,
    ProtocolError,
    get_message_type,
    read_message,
    request_id,
    write_message,
)
from .runtime import HostAPI, LuaError, Sandbox, from_jsonable, to_jsonable


class HostError(Exception):
    """A host descriptor or request could not be handled."""


@dataclass(frozen=True)
class HostDescriptor:
    """Everything a controller needs to attach to one explicit test host."""

    host: str
    port: int
    host_id: str
    name: str
    token: str
    protocol: int = PROTOCOL_VERSION
    capabilities: tuple[str, ...] = (
        "execute",
        "validate",
        "state.read",
        "state.write",
        "events",
        "logging",
    )

    def to_dict(self) -> dict[str, Any]:
        return {
            "protocol": self.protocol,
            "host": self.host,
            "port": self.port,
            "host_id": self.host_id,
            "name": self.name,
            "token": self.token,
            "capabilities": list(self.capabilities),
        }

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> HostDescriptor:
        try:
            host = value["host"]
            port = value["port"]
            host_id = value["host_id"]
            name = value["name"]
            token = value["token"]
        except KeyError as exc:
            raise HostError(f"descriptor is missing {exc.args[0]!r}") from exc
        if not isinstance(host, str) or host not in {"127.0.0.1", "localhost"}:
            raise HostError("only loopback hosts are allowed")
        if not isinstance(port, int) or not 1 <= port <= 65_535:
            raise HostError("descriptor port is invalid")
        if not all(isinstance(item, str) for item in (host_id, name, token)):
            raise HostError("descriptor identity fields must be strings")
        capabilities = value.get("capabilities", [])
        if not isinstance(capabilities, list) or not all(isinstance(item, str) for item in capabilities):
            raise HostError("descriptor capabilities must be a list of strings")
        return cls(
            host=host,
            port=port,
            host_id=host_id,
            name=name,
            token=token,
            protocol=int(value.get("protocol", PROTOCOL_VERSION)),
            capabilities=tuple(capabilities),
        )

    @classmethod
    def read(cls, path: str | Path) -> HostDescriptor:
        import json

        descriptor_path = Path(path)
        try:
            value = json.loads(descriptor_path.read_text(encoding="utf-8"))
        except FileNotFoundError as exc:
            raise HostError(f"descriptor not found: {descriptor_path}") from exc
        except (OSError, json.JSONDecodeError) as exc:
            raise HostError(f"could not read descriptor {descriptor_path}: {exc}") from exc
        if not isinstance(value, dict):
            raise HostError("descriptor must contain a JSON object")
        return cls.from_dict(value)

    def write(self, path: str | Path) -> None:
        import json
        import os

        descriptor_path = Path(path)
        descriptor_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = descriptor_path.with_suffix(descriptor_path.suffix + ".tmp")
        temporary.write_text(
            json.dumps(self.to_dict(), indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        try:
            os.replace(temporary, descriptor_path)
            # The descriptor contains the session token.  Keep it private on
            # POSIX systems; the host is still protected by authentication if
            # a platform ignores this mode change.
            try:
                os.chmod(descriptor_path, 0o600)
            except OSError:
                pass
        except OSError:
            temporary.unlink(missing_ok=True)
            raise


class _ThreadingTCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class HostService:
    """Own the test-host state and expose it over a local socket."""

    MAX_TIMEOUT_MS = 30_000
    MAX_INSTRUCTIONS = 1_000_000

    def __init__(
        self,
        *,
        name: str = "Lua Injection Test Host",
        host: str = "127.0.0.1",
        port: int = 0,
        token: str | None = None,
        host_id: str | None = None,
        initial_state: dict[str, Any] | None = None,
    ):
        if host not in {"127.0.0.1", "localhost"}:
            raise HostError("Lua Injection hosts must bind to loopback")
        if not 0 <= port <= 65_535:
            raise HostError("port must be between 0 and 65535")
        if not name or len(name) > 100:
            raise HostError("host name must be between 1 and 100 characters")
        self.name = name
        self.bind_host = host
        self.token = token or secrets.token_urlsafe(24)
        self.host_id = host_id or f"host-{uuid.uuid4().hex[:12]}"
        self.initial_state = dict(initial_state or {"counter": 0, "status": "ready"})
        self.lock = threading.RLock()
        self.started_at = time.monotonic()
        self.logs: list[dict[str, Any]] = []
        self.host_api = HostAPI(
            name=self.name,
            host_id=self.host_id,
            state=self.initial_state,
            logger=self._log,
        )
        try:
            self.server = _ThreadingTCPServer((self.bind_host, port), HostRequestHandler)
        except OSError as exc:
            raise HostError(f"could not bind {self.bind_host}:{port}: {exc}") from exc
        self.server.service = self  # type: ignore[attr-defined]
        actual_host, actual_port = self.server.server_address[:2]
        descriptor_host = "127.0.0.1" if actual_host in {"localhost", "::1"} else actual_host
        self.descriptor = HostDescriptor(
            host=descriptor_host,
            port=int(actual_port),
            host_id=self.host_id,
            name=self.name,
            token=self.token,
        )

    @property
    def address(self) -> str:
        return f"{self.descriptor.host}:{self.descriptor.port}"

    def serve_forever(self) -> None:
        self.server.serve_forever(poll_interval=0.2)

    def shutdown(self) -> None:
        self.server.shutdown()
        self.server.server_close()

    def _log(self, level: str, message: str) -> None:
        with self.lock:
            self.logs.append({"level": level, "message": message, "at_ms": self.elapsed_ms()})
            del self.logs[:-100]

    def elapsed_ms(self) -> int:
        return int((time.monotonic() - self.started_at) * 1000)

    def state_snapshot(self) -> dict[str, Any]:
        with self.lock:
            return to_jsonable(self.host_api.state)

    def reset(self) -> dict[str, Any]:
        with self.lock:
            self.host_api.state = from_jsonable(self.initial_state)
            self.host_api.events.clear()
            self.logs.clear()
            return to_jsonable(self.host_api.state)

    def execute(
        self,
        script: str,
        *,
        timeout_ms: int = 2_000,
        instruction_limit: int = 100_000,
        dry_run: bool = False,
    ) -> dict[str, Any]:
        if not isinstance(script, str):
            raise HostError("execute.script must be a string")
        if not 1 <= timeout_ms <= self.MAX_TIMEOUT_MS:
            raise HostError(f"timeout_ms must be between 1 and {self.MAX_TIMEOUT_MS}")
        if not 1 <= instruction_limit <= self.MAX_INSTRUCTIONS:
            raise HostError(f"instruction_limit must be between 1 and {self.MAX_INSTRUCTIONS}")
        with self.lock:
            if dry_run:
                try:
                    Sandbox(
                        self.host_api,
                        max_instructions=instruction_limit,
                        timeout_ms=timeout_ms,
                    ).validate(script)
                    return {"status": "ok", "validated": True, "error": None}
                except LuaError as exc:
                    return {"status": "error", "validated": False, "error": str(exc)}
            self.host_api.events.clear()
            log_start = len(self.logs)
            result = Sandbox(
                self.host_api,
                max_instructions=instruction_limit,
                timeout_ms=timeout_ms,
            ).execute(script)
            result["logs"] = list(self.logs[log_start:])
            result["state"] = to_jsonable(self.host_api.state)
            return result

    def request(self, message: dict[str, Any]) -> dict[str, Any]:
        """Handle one authenticated request and return its response payload."""
        message_type = get_message_type(message)
        if message_type == "ping":
            return {
                "alive": True,
                "host_id": self.host_id,
                "name": self.name,
                "uptime_ms": self.elapsed_ms(),
                "capabilities": list(self.descriptor.capabilities),
            }
        if message_type == "get_state":
            return {"state": self.state_snapshot(), "logs": list(self.logs[-100:])}
        if message_type == "reset_state":
            return {"state": self.reset()}
        if message_type == "validate":
            return self.execute(
                message.get("script", ""),
                timeout_ms=int(message.get("timeout_ms", 2_000)),
                instruction_limit=int(message.get("instruction_limit", 100_000)),
                dry_run=True,
            )
        if message_type == "execute":
            return self.execute(
                message.get("script", ""),
                timeout_ms=int(message.get("timeout_ms", 2_000)),
                instruction_limit=int(message.get("instruction_limit", 100_000)),
                dry_run=False,
            )
        if message_type == "detach":
            return {"detached": True}
        raise HostError(f"unknown request type {message_type!r}")


class HostRequestHandler(socketserver.StreamRequestHandler):
    """Authenticate one controller and serve newline-delimited requests."""

    def handle(self) -> None:
        service: HostService = self.server.service  # type: ignore[attr-defined]
        self.connection.settimeout(35)
        authenticated = False
        try:
            write_message(
                self.wfile,
                {
                    "type": "hello",
                    "protocol": PROTOCOL_VERSION,
                    "host_id": service.host_id,
                    "name": service.name,
                    "capabilities": list(service.descriptor.capabilities),
                    "auth": "token",
                },
            )
            auth_attempts = 0
            while True:
                message = read_message(self.rfile)
                if message is None:
                    return
                message_type = get_message_type(message)
                if not authenticated:
                    if message_type != "auth":
                        raise ProtocolError("first request must be auth")
                    auth_attempts += 1
                    token = message.get("token")
                    if not isinstance(token, str) or not hmac.compare_digest(token, service.token):
                        if auth_attempts >= 3:
                            raise ProtocolError("authentication failed")
                        write_message(self.wfile, {"type": "auth_error", "error": "invalid token"})
                        continue
                    authenticated = True
                    write_message(
                        self.wfile,
                        {
                            "type": "auth_ok",
                            "protocol": PROTOCOL_VERSION,
                            "host_id": service.host_id,
                        },
                    )
                    continue
                if message_type == "auth":
                    raise ProtocolError("already authenticated")
                identifier = request_id(message)
                try:
                    result = service.request(message)
                    write_message(
                        self.wfile,
                        {"type": "response", "id": identifier, "ok": True, "result": result},
                    )
                    if message_type == "detach":
                        return
                except (HostError, LuaError, ValueError, TypeError) as exc:
                    write_message(
                        self.wfile,
                        {"type": "response", "id": identifier, "ok": False, "error": str(exc)},
                    )
        except (ConnectionError, OSError, ProtocolError, socket.timeout):
            return


__all__ = ["HostDescriptor", "HostError", "HostService"]
