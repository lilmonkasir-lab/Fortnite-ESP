# Lua Injection

**Lua Injection** is a small, zero-dependency Lua test harness for applications you
own. It makes attaching to a test host a one-command workflow while still giving
advanced users a script API, state, events, logs, validation, watch mode, and
execution limits.

The repository was accidentally started with the `Fortnite-ESP` name. The
project branding, package name, CLI, documentation, and generated workspace
are now **Lua Injection**.

> **Safety boundary:** this is not a DLL injector, game cheat, memory patcher, or
> arbitrary-process attach tool. A host must explicitly start
> `lua-injection host`, binds to loopback, and proves its identity with a
> generated token. Scripts run in a restricted Lua subset and cannot access
> files, processes, sockets, `os`, `io`, `debug`, `package`, or dynamic code
> loading. Use it only with test applications and hosts you control.

## Why this is simpler and smarter

- **One setup command:** `lua-injection init` creates a workspace and a safe
  starter script.
- **Explicit attaching:** a host writes a short-lived JSON descriptor containing
  its loopback address, identity, capabilities, and token. The client verifies
  the protocol and host identity before it runs anything.
- **Useful by default, advanced when needed:** run locally with `run`, attach to
  a real test host with `attach`, inspect it with `state`, or use the Python API.
- **Safer execution:** every script has a source-size limit, instruction budget,
  timeout, and output limit. Infinite loops fail instead of hanging a host.
- **Host methods:** read and write test state, apply a patch, emit an event, log
  at a level, get a snapshot, inspect capabilities, and read host time.
- **Fast iteration:** `--watch` re-runs a script whenever its file changes.
- **Automation-friendly:** `--json` produces stable machine-readable responses;
  the wire protocol is newline-delimited JSON and versioned.

## Quick start

Python 3.10 or newer is required. The project has no runtime dependencies.

```bash
python -m venv .venv
. .venv/bin/activate  # Windows PowerShell: .venv\Scripts\Activate.ps1
python -m pip install -e .
lua-injection init
```

For a no-install checkout, prefix commands with `PYTHONPATH=src` and run
`python -m lua_injection ...`.

In terminal 1, start the explicitly controlled test host:

```bash
lua-injection host
```

In terminal 2, attach and run the generated script:

```bash
lua-injection attach --script scripts/hello.lua --show-state
```

You should see output similar to:

```text
attached to Lua Injection Test Host (host-...)
status: ok
output:
  connected to Lua Injection Test Host
  counter is now 1
events:
  counter.changed: {"value": 1}
state: {"counter": 1, "status": "ready"}
```

The host removes its descriptor on shutdown. A descriptor is a local session
credential; do not commit or share it.

## Commands

```text
lua-injection init [--path DIR] [--force]
lua-injection run --script FILE | --code TEXT
lua-injection validate --script FILE | --code TEXT
lua-injection host [--name NAME] [--port PORT] [--descriptor FILE]
lua-injection attach [--descriptor FILE] [--script FILE | --code TEXT]
lua-injection state [--descriptor FILE] [--reset]
```

Useful options:

- `--json` on `run`, `host`, `attach`, and `state` for scripts and CI.
- `--timeout-ms` and `--instruction-limit` on `run` and `attach`.
- `--dry-run` on `attach` to validate on the host without changing state.
- `--watch` on `attach --script FILE` for a small edit-run loop.
- `--retries N` on `attach` for a host that is still starting.
- `--show-state` on `attach` to include the post-run state.

For a temporary CI host, choose a free port and stop automatically:

```bash
lua-injection host --port 0 --duration 10 --json
```

For a direct attach without a descriptor, all three values are required and the
host is still restricted to loopback:

```bash
lua-injection attach \
  --host 127.0.0.1 --port 45678 --token 'printed-by-host' \
  --code "print(host.name())"
```

Omit `--script` and `--code` to open the interactive session:

```text
lua> :state
lua> print(host.get('status'))
lua> :detach
```

## Script API

The built-in runtime supports common Lua statements and expressions including
locals, functions, conditionals, `while`, numeric and generic `for` loops,
tables, `pairs`, `ipairs`, arithmetic, comparisons, string concatenation,
`math`, `string`, `table`, and `json` helpers.

Only the following host bridge is exposed:

```lua
print('host: ' .. host.name())
print('id: ' .. host.id())

local old = host.get('counter') or 0
host.set('counter', old + 1)
host.patch({ status = 'tested', last_value = old + 1 })
host.emit('counter.changed', { value = old + 1 })
host.log('info', 'counter updated')

local snapshot = host.snapshot()
print(json.encode(snapshot))
```

Methods:

| Method | Purpose |
| --- | --- |
| `host.name()` | Test host display name |
| `host.id()` | Stable host session id |
| `host.get(key)` | Read one JSON-compatible state value |
| `host.set(key, value)` | Write one bounded state value |
| `host.patch(table)` | Write several string-keyed values |
| `host.snapshot()` | Copy the current state |
| `host.emit(name, payload)` | Record a structured test event |
| `host.log(level, message)` | Add a host log entry (`debug`, `info`, `warn`, `error`) |
| `host.capabilities()` | Return the advertised safe capabilities |
| `host.time_ms()` | Milliseconds since the host started |

## Embedding a host in a test application

A test application can start a host directly instead of using the CLI:

```python
from lua_injection.host import HostService

service = HostService(
    name="My component test host",
    initial_state={"counter": 0, "status": "ready"},
)
print(service.descriptor.to_dict())
try:
    service.serve_forever()
finally:
    service.shutdown()
```

A controller can use the same authenticated lifecycle:

```python
from lua_injection.client import LuaInjectionClient

with LuaInjectionClient(service.descriptor) as session:
    result = session.execute("host.emit('smoke_test', { ok = true })")
    print(result["status"])
```

The host and client communicate with version 1 of a line-delimited JSON
protocol. The initial `hello` is followed by token `auth`; requests include an
`id` and receive a matching `response`. This makes it straightforward to
write a small adapter for another test runner without giving that adapter
process-level powers.

## Development

```bash
PYTHONPATH=src python -m unittest discover -s tests -v
PYTHONPATH=src python -m lua_injection --help
```

The tests cover the sandbox, limits, protocol validation, authentication, and
an end-to-end host/client session.

## License

MIT. See [LICENSE](LICENSE).
