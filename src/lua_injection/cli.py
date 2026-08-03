"""Command-line interface for the Lua Injection test harness."""

from __future__ import annotations

import argparse
import json
import sys
import threading
import time
from pathlib import Path
from typing import Any, Sequence

from . import __version__
from .client import AttachError, LuaInjectionClient
from .host import HostDescriptor, HostError, HostService
from .runtime import HostAPI, LuaError, Sandbox

DEFAULT_DESCRIPTOR = Path(".lua-injection/host.json")


def _add_execution_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--timeout-ms",
        type=int,
        default=2_000,
        help="wall-clock budget checked by the sandbox (default: 2000)",
    )
    parser.add_argument(
        "--instruction-limit",
        type=int,
        default=100_000,
        help="maximum interpreter steps (default: 100000)",
    )


def _add_script_options(parser: argparse.ArgumentParser) -> None:
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--script", type=Path, help="read Lua source from a file")
    group.add_argument("--code", help="execute an inline Lua snippet")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="lua-injection",
        description="Simple, safe Lua automation for explicitly controlled test hosts.",
        epilog="This tool attaches only to a loopback host started by `lua-injection host`.",
    )
    parser.add_argument("--version", action="version", version=f"Lua Injection {__version__}")
    subparsers = parser.add_subparsers(dest="command", required=True)

    init_parser = subparsers.add_parser("init", help="create a ready-to-use workspace")
    init_parser.add_argument("--path", type=Path, default=Path("."), help="workspace directory (default: .)")
    init_parser.add_argument("--force", action="store_true", help="replace generated files")
    init_parser.set_defaults(handler=command_init)

    run_parser = subparsers.add_parser("run", help="run Lua locally in the safe sandbox")
    _add_script_options(run_parser)
    _add_execution_options(run_parser)
    run_parser.add_argument("--json", action="store_true", help="print one machine-readable JSON result")
    run_parser.set_defaults(handler=command_run)

    validate_parser = subparsers.add_parser("validate", help="parse a script without running it")
    _add_script_options(validate_parser)
    validate_parser.set_defaults(handler=command_validate)

    host_parser = subparsers.add_parser("host", help="start an explicit loopback test host")
    host_parser.add_argument("--name", default="Lua Injection Test Host", help="display name")
    host_parser.add_argument("--host", default="127.0.0.1", help="loopback bind address")
    host_parser.add_argument("--port", type=int, default=0, help="TCP port; 0 chooses a free port")
    host_parser.add_argument("--token", help="use a supplied token instead of generating one")
    host_parser.add_argument(
        "--state-json",
        help='initial JSON object, for example \'{"counter":0,"status":"ready"}\'',
    )
    host_parser.add_argument(
        "--descriptor",
        type=Path,
        default=DEFAULT_DESCRIPTOR,
        help=f"write attach descriptor here (default: {DEFAULT_DESCRIPTOR})",
    )
    host_parser.add_argument("--no-descriptor", action="store_true", help="do not write a descriptor file")
    host_parser.add_argument("--json", action="store_true", help="print descriptor as JSON")
    host_parser.add_argument(
        "--duration",
        type=float,
        default=0,
        help="stop automatically after this many seconds (useful in CI; default: forever)",
    )
    host_parser.set_defaults(handler=command_host)

    attach_parser = subparsers.add_parser("attach", help="attach to a descriptor and execute a script")
    attach_script_group = attach_parser.add_mutually_exclusive_group(required=False)
    attach_script_group.add_argument("--script", type=Path, help="read Lua source from a file")
    attach_script_group.add_argument("--code", help="execute an inline Lua snippet")
    attach_parser.add_argument(
        "--descriptor",
        type=Path,
        default=DEFAULT_DESCRIPTOR,
        help=f"host descriptor (default: {DEFAULT_DESCRIPTOR})",
    )
    attach_parser.add_argument("--host", help="loopback host; use with --port and --token instead of a descriptor")
    attach_parser.add_argument("--port", type=int, help="host port for direct attach")
    attach_parser.add_argument("--token", help="host token for direct attach")
    attach_parser.add_argument("--retries", type=int, default=3, help="initial connection attempts (default: 3)")
    attach_parser.add_argument("--watch", action="store_true", help="re-run the file whenever it changes")
    attach_parser.add_argument("--dry-run", action="store_true", help="validate on the host without executing")
    attach_parser.add_argument("--show-state", action="store_true", help="print host state after execution")
    attach_parser.add_argument("--json", action="store_true", help="print result objects as JSON")
    _add_execution_options(attach_parser)
    attach_parser.set_defaults(handler=command_attach)

    state_parser = subparsers.add_parser("state", help="inspect or reset an attached host")
    state_parser.add_argument("--descriptor", type=Path, default=DEFAULT_DESCRIPTOR)
    state_parser.add_argument("--host")
    state_parser.add_argument("--port", type=int)
    state_parser.add_argument("--token")
    state_parser.add_argument("--reset", action="store_true", help="restore the host's initial state")
    state_parser.add_argument("--json", action="store_true", help="print JSON")
    state_parser.set_defaults(handler=command_state)

    return parser


def _source_from_args(args: argparse.Namespace) -> str:
    if getattr(args, "code", None) is not None:
        return args.code
    if getattr(args, "script", None) is None:
        raise HostError("provide --script or --code")
    try:
        return args.script.read_text(encoding="utf-8")
    except OSError as exc:
        raise HostError(f"could not read {args.script}: {exc}") from exc


def _print_json(value: Any) -> None:
    print(json.dumps(value, indent=2, ensure_ascii=False, sort_keys=True))


def _print_execution(result: dict[str, Any], *, as_json: bool = False) -> int:
    if as_json:
        _print_json(result)
    else:
        status = result.get("status", "unknown")
        print(f"status: {status}")
        if result.get("output"):
            print("output:")
            for line in result["output"]:
                print(f"  {line}")
        if result.get("events"):
            print("events:")
            for event in result["events"]:
                print(f"  {event.get('name')}: {json.dumps(event.get('payload'))}")
        if result.get("logs"):
            print("logs:")
            for entry in result["logs"]:
                print(f"  [{entry.get('level')}] {entry.get('message')}")
        if "state" in result:
            print(f"state: {json.dumps(result['state'], sort_keys=True)}")
        if result.get("result") is not None:
            print(f"result: {json.dumps(result['result'], ensure_ascii=False)}")
        if result.get("instructions") is not None:
            print(f"timing: {result['duration_ms']} ms, {result['instructions']} instructions")
        if result.get("error"):
            print(f"error: {result['error']}", file=sys.stderr)
    return 0 if result.get("status") == "ok" else 1


def command_init(args: argparse.Namespace) -> int:
    root = args.path.resolve()
    config_path = root / ".lua-injection" / "config.json"
    script_path = root / "scripts" / "hello.lua"
    config = {
        "name": "Lua Injection workspace",
        "descriptor": ".lua-injection/host.json",
        "scripts": "scripts",
        "defaults": {"timeout_ms": 2000, "instruction_limit": 100000},
    }
    template = """-- A safe first script: it talks only to the attached test host.\n"""
    template += "print('connected to ' .. host.name())\n"
    template += "local before = host.get('counter') or 0\n"
    template += "host.set('counter', before + 1)\n"
    template += "host.emit('counter.changed', { value = before + 1 })\n"
    template += "print('counter is now ' .. tostring(host.get('counter')))\n"
    for path, content in ((config_path, json.dumps(config, indent=2) + "\n"), (script_path, template)):
        if path.exists() and not args.force:
            print(f"skip {path} (already exists; use --force to replace)")
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        print(f"created {path}")
    print("\nNext steps:")
    print("  lua-injection host")
    print("  lua-injection attach --script scripts/hello.lua --show-state")
    return 0


def command_run(args: argparse.Namespace) -> int:
    source = _source_from_args(args)
    sandbox = Sandbox(
        HostAPI(name="local-sandbox", host_id="local"),
        max_instructions=args.instruction_limit,
        timeout_ms=args.timeout_ms,
    )
    return _print_execution(sandbox.execute(source), as_json=args.json)


def command_validate(args: argparse.Namespace) -> int:
    source = _source_from_args(args)
    try:
        Sandbox().validate(source)
    except LuaError as exc:
        print(f"invalid: {exc}", file=sys.stderr)
        return 1
    print("valid: Lua Injection subset")
    return 0


def _descriptor_from_args(args: argparse.Namespace) -> HostDescriptor:
    direct_values = (args.host, args.port, args.token)
    if any(value is not None for value in direct_values):
        if not all(value is not None for value in direct_values):
            raise HostError("direct attach requires --host, --port, and --token together")
        if args.host not in {"127.0.0.1", "localhost"}:
            raise HostError("only loopback hosts are allowed")
        if not isinstance(args.port, int) or not 1 <= args.port <= 65_535:
            raise HostError("direct attach port must be between 1 and 65535")
        return HostDescriptor(
            host=args.host,
            port=args.port,
            host_id=getattr(args, "host_id", "") or "",
            name="direct host",
            token=args.token,
        )
    return HostDescriptor.read(args.descriptor)


def _attach_client(args: argparse.Namespace) -> LuaInjectionClient:
    return LuaInjectionClient(_descriptor_from_args(args))


def _run_attached_once(
    client: LuaInjectionClient,
    source: str,
    args: argparse.Namespace,
) -> int:
    result = (
        client.validate(source, timeout_ms=args.timeout_ms, instruction_limit=args.instruction_limit)
        if args.dry_run
        else client.execute(source, timeout_ms=args.timeout_ms, instruction_limit=args.instruction_limit)
    )
    if args.dry_run and result.get("status") == "ok":
        result = {"status": "ok", "validated": True, **result}
    if args.show_state and not args.dry_run:
        state = client.get_state()
        result["state"] = state.get("state", state)
        result["logs"] = state.get("logs", [])
    return _print_execution(result, as_json=args.json)


def _interactive(client: LuaInjectionClient, as_json: bool) -> int:
    print("Interactive session. Commands: :state, :ping, :reset, :help, :detach")
    while True:
        try:
            line = input("lua> ")
        except EOFError:
            print()
            return 0
        command = line.strip()
        if not command:
            continue
        if command in {":detach", ":quit", ":q"}:
            return 0
        if command == ":help":
            print("Enter a Lua snippet, or use :state, :ping, :reset, :detach.")
            continue
        try:
            if command == ":state":
                value = client.get_state()
                _print_json(value) if as_json else print(json.dumps(value, indent=2, sort_keys=True))
            elif command == ":ping":
                value = client.ping()
                _print_json(value) if as_json else print(f"alive: {value.get('alive')} ({value.get('uptime_ms')} ms)")
            elif command == ":reset":
                value = client.reset_state()
                _print_json(value) if as_json else print(f"state reset: {json.dumps(value.get('state'))}")
            else:
                result = client.execute(command)
                _print_execution(result, as_json=as_json)
        except AttachError as exc:
            print(f"attach error: {exc}", file=sys.stderr)
            return 2


def command_attach(args: argparse.Namespace) -> int:
    if args.watch and args.code is not None:
        raise HostError("--watch requires --script")
    if args.watch and args.script is None:
        raise HostError("--watch requires --script")
    source = _source_from_args(args) if args.code is not None or args.script is not None else ""
    client = _attach_client(args)
    try:
        hello = client.connect_with_retry(retries=args.retries)
        if not args.json:
            print(f"attached to {hello.get('name')} ({hello.get('host_id')})")
            print("capabilities: " + ", ".join(hello.get("capabilities", [])))
        if args.watch:
            assert args.script is not None
            last_mtime: float | None = None
            exit_code = 0
            while True:
                mtime = args.script.stat().st_mtime
                if last_mtime is None or mtime != last_mtime:
                    last_mtime = mtime
                    source = _source_from_args(args)
                    exit_code = _run_attached_once(client, source, args)
                time.sleep(0.25)
        elif args.code is None and args.script is None:
            return _interactive(client, args.json)
        return _run_attached_once(client, source, args)
    finally:
        client.detach()


def command_state(args: argparse.Namespace) -> int:
    client = _attach_client(args)
    try:
        client.connect_with_retry()
        value = client.reset_state() if args.reset else client.get_state()
        if args.json:
            _print_json(value)
        else:
            print(json.dumps(value, indent=2, sort_keys=True))
        return 0
    finally:
        client.detach()


def command_host(args: argparse.Namespace) -> int:
    state: dict[str, Any] | None = None
    if args.state_json:
        try:
            parsed = json.loads(args.state_json)
        except json.JSONDecodeError as exc:
            raise HostError(f"--state-json is invalid JSON: {exc}") from exc
        if not isinstance(parsed, dict):
            raise HostError("--state-json must be a JSON object")
        state = parsed
    service = HostService(
        name=args.name,
        host=args.host,
        port=args.port,
        token=args.token,
        initial_state=state,
    )
    if not args.no_descriptor:
        service.descriptor.write(args.descriptor)
    if args.json:
        _print_json(service.descriptor.to_dict())
    else:
        print("Lua Injection test host ready")
        print(f"  address:    {service.address}")
        print(f"  host id:    {service.descriptor.host_id}")
        print(f"  token:      {service.token}")
        if not args.no_descriptor:
            print(f"  descriptor: {args.descriptor}")
        print("  stop with Ctrl+C")
    server_thread: threading.Thread | None = None
    try:
        if args.duration > 0:
            # serve_forever must run on a separate thread while the CLI waits
            # for the deadline; otherwise --duration would create a descriptor
            # for a server that never accepts connections.
            server_thread = threading.Thread(target=service.serve_forever, daemon=True)
            server_thread.start()
            deadline = time.monotonic() + args.duration
            while time.monotonic() < deadline:
                time.sleep(min(0.25, deadline - time.monotonic()))
        else:
            service.serve_forever()
    except KeyboardInterrupt:
        if not args.json:
            print("\nstopping host")
    finally:
        service.shutdown()
        if server_thread is not None:
            server_thread.join(timeout=2)
        if not args.no_descriptor:
            args.descriptor.unlink(missing_ok=True)
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.handler(args))
    except (AttachError, HostError, LuaError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("cancelled", file=sys.stderr)
        return 130


__all__ = ["build_parser", "main"]
