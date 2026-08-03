"""A small, deliberately restricted Lua runtime used by Lua Injection.

The project is a test harness, not a process injector.  Scripts run inside this
module (or inside a host that imports this module) and only receive the safe
``host`` API.  There is intentionally no ``os``, ``io``, ``debug``, ``package``
or dynamic code-loading library.

This is a useful Lua-compatible subset for smoke tests and host automation. If
full Lua is available in a downstream application, the protocol can be used
with that application instead; the built-in runtime keeps this repository
zero-dependency and easy to try.
"""

from __future__ import annotations

import json
import math
import random
import re
import time
from dataclasses import dataclass
from typing import Any, Callable, Iterable, Iterator, Mapping, Sequence


class LuaError(Exception):
    """Base class for parser and runtime errors exposed to the user."""


class LuaSyntaxError(LuaError):
    """The supplied script is not valid for the supported Lua subset."""


class LuaRuntimeError(LuaError):
    """The script could not be executed safely."""


class ExecutionLimit(LuaRuntimeError):
    """The instruction or wall-clock budget was exceeded."""


@dataclass(frozen=True)
class Token:
    kind: str
    value: Any
    line: int
    column: int


class Lexer:
    """Tokenize the small Lua subset accepted by :class:`Sandbox`."""

    _number = re.compile(r"(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?")
    _name = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
    _multi_symbols = ("...", "==", "~=", "<=", ">=", "..")

    def __init__(self, source: str):
        self.source = source
        self.pos = 0
        self.line = 1
        self.column = 1
        self.tokens: list[Token] = []

    def _advance(self, count: int = 1) -> str:
        text = self.source[self.pos : self.pos + count]
        for char in text:
            if char == "\n":
                self.line += 1
                self.column = 1
            else:
                self.column += 1
        self.pos += count
        return text

    def _error(self, message: str, line: int | None = None, column: int | None = None) -> LuaSyntaxError:
        return LuaSyntaxError(
            f"{message} at line {line or self.line}, column {column or self.column}"
        )

    def _skip_space_and_comments(self) -> None:
        while self.pos < len(self.source):
            if self.source[self.pos].isspace():
                self._advance()
                continue
            if self.source.startswith("--", self.pos):
                comment_line, comment_column = self.line, self.column
                if self.source.startswith("--[[", self.pos):
                    self._advance(4)
                    end = self.source.find("]]", self.pos)
                    if end == -1:
                        raise self._error("unterminated block comment", comment_line, comment_column)
                    self._advance(end - self.pos + 2)
                else:
                    while self.pos < len(self.source) and self.source[self.pos] != "\n":
                        self._advance()
                continue
            break

    def _read_string(self) -> Token:
        start_line, start_column = self.line, self.column
        quote = self.source[self.pos]
        self._advance()
        result: list[str] = []
        escapes = {
            "a": "\a",
            "b": "\b",
            "f": "\f",
            "n": "\n",
            "r": "\r",
            "t": "\t",
            "v": "\v",
            "\\": "\\",
            '"': '"',
            "'": "'",
        }
        while self.pos < len(self.source):
            char = self.source[self.pos]
            if char == quote:
                self._advance()
                return Token("string", "".join(result), start_line, start_column)
            if char == "\n":
                raise self._error("newline in string literal", start_line, start_column)
            if char != "\\":
                result.append(self._advance())
                continue
            self._advance()
            if self.pos >= len(self.source):
                break
            escaped = self.source[self.pos]
            if escaped.isdigit():
                digits = []
                for _ in range(3):
                    if self.pos < len(self.source) and self.source[self.pos].isdigit():
                        digits.append(self._advance())
                    else:
                        break
                value = int("".join(digits), 10)
                if value > 255:
                    raise self._error("numeric escape must be between 0 and 255", start_line, start_column)
                result.append(chr(value))
            elif escaped == "\n":
                result.append("\n")
                self._advance()
            elif escaped in escapes:
                result.append(escapes[escaped])
                self._advance()
            else:
                # Lua accepts escaped punctuation; keeping the character is the
                # least surprising behavior for a test harness.
                result.append(self._advance())
        raise self._error("unterminated string literal", start_line, start_column)

    def _read_long_string(self) -> Token:
        start_line, start_column = self.line, self.column
        self._advance(2)  # [[
        end = self.source.find("]]", self.pos)
        if end == -1:
            raise self._error("unterminated long string", start_line, start_column)
        value = self.source[self.pos : end]
        self._advance(end - self.pos + 2)
        return Token("string", value, start_line, start_column)

    def tokenize(self) -> list[Token]:
        single_symbols = set("+-*/%^#<>=(){}[];,.:~")
        while True:
            self._skip_space_and_comments()
            if self.pos >= len(self.source):
                self.tokens.append(Token("eof", "<eof>", self.line, self.column))
                return self.tokens

            line, column = self.line, self.column
            if self.source.startswith("[[", self.pos):
                self.tokens.append(self._read_long_string())
                continue
            char = self.source[self.pos]
            if char in "'\"":
                self.tokens.append(self._read_string())
                continue
            match = self._number.match(self.source, self.pos)
            if match:
                raw = match.group(0)
                self._advance(len(raw))
                number: int | float
                number = float(raw) if any(c in raw for c in ".eE") else int(raw)
                self.tokens.append(Token("number", number, line, column))
                continue
            match = self._name.match(self.source, self.pos)
            if match:
                raw = match.group(0)
                self._advance(len(raw))
                self.tokens.append(Token("name", raw, line, column))
                continue
            found = False
            for symbol in self._multi_symbols:
                if self.source.startswith(symbol, self.pos):
                    self._advance(len(symbol))
                    self.tokens.append(Token("symbol", symbol, line, column))
                    found = True
                    break
            if found:
                continue
            if char in single_symbols:
                self._advance()
                self.tokens.append(Token("symbol", char, line, column))
                continue
            raise self._error(f"unexpected character {char!r}", line, column)


class Parser:
    """Recursive-descent / Pratt parser for the supported Lua subset."""

    _precedence = {
        "or": (1, False),
        "and": (2, False),
        "==": (3, False),
        "~=": (3, False),
        "<": (3, False),
        ">": (3, False),
        "<=": (3, False),
        ">=": (3, False),
        "..": (4, True),
        "+": (5, False),
        "-": (5, False),
        "*": (6, False),
        "/": (6, False),
        "%": (6, False),
        "^": (7, True),
    }

    def __init__(self, tokens: Sequence[Token]):
        self.tokens = tokens
        self.index = 0

    @property
    def current(self) -> Token:
        return self.tokens[self.index]

    def peek(self, value: str | None = None, offset: int = 0) -> bool:
        token = self.tokens[min(self.index + offset, len(self.tokens) - 1)]
        return value is None or token.value == value

    def consume(self) -> Token:
        token = self.current
        if token.kind != "eof":
            self.index += 1
        return token

    def accept(self, value: str) -> Token | None:
        if self.peek(value):
            return self.consume()
        return None

    def expect(self, value: str) -> Token:
        if not self.peek(value):
            raise self.error(f"expected {value!r}, got {self.current.value!r}")
        return self.consume()

    def error(self, message: str) -> LuaSyntaxError:
        token = self.current
        return LuaSyntaxError(f"{message} at line {token.line}, column {token.column}")

    def parse(self) -> list[tuple[Any, ...]]:
        program = self.parse_block(set())
        if self.current.kind != "eof":
            raise self.error(f"unexpected {self.current.value!r}")
        return program

    def parse_block(self, terminators: set[str]) -> list[tuple[Any, ...]]:
        statements: list[tuple[Any, ...]] = []
        while self.current.kind != "eof" and self.current.value not in terminators:
            if self.accept(";"):
                continue
            statements.append(self.parse_statement())
            self.accept(";")
        return statements

    def parse_statement(self) -> tuple[Any, ...]:
        value = self.current.value
        if value == "local":
            self.consume()
            if self.accept("function"):
                name = self.expect_name().value
                function = self.parse_function_body()
                return ("local_function", name, function)
            names = [self.expect_name().value]
            while self.accept(","):
                names.append(self.expect_name().value)
            expressions = self.parse_expression_list() if self.accept("=") else []
            return ("local", names, expressions)
        if value == "function":
            self.consume()
            target = self.parse_function_target()
            function = self.parse_function_body()
            return ("assign", [target], [function])
        if value == "if":
            return self.parse_if()
        if value == "while":
            return self.parse_while()
        if value == "for":
            return self.parse_for()
        if value == "do":
            self.consume()
            body = self.parse_block({"end"})
            self.expect("end")
            return ("block", body)
        if value == "return":
            self.consume()
            if self.current.value in {"end", "else", "elseif", ";"} or self.current.kind == "eof":
                return ("return", [])
            return ("return", self.parse_expression_list())
        if value == "break":
            self.consume()
            return ("break",)

        first = self.parse_expression()
        if self.peek("=") or self.peek(","):
            targets = [first]
            while self.accept(","):
                targets.append(self.parse_expression())
            self.expect("=")
            if any(not self.is_assignable(target) for target in targets):
                raise self.error("only names and table fields can be assigned")
            return ("assign", targets, self.parse_expression_list())
        if first[0] != "call" and first[0] != "method_call":
            raise self.error("a statement must be a function call or assignment")
        return ("expr", first)

    def parse_if(self) -> tuple[Any, ...]:
        branches: list[tuple[tuple[Any, ...], list[tuple[Any, ...]]]] = []
        self.expect("if")
        condition = self.parse_expression()
        self.expect("then")
        branches.append((condition, self.parse_block({"elseif", "else", "end"})))
        while self.accept("elseif"):
            condition = self.parse_expression()
            self.expect("then")
            branches.append((condition, self.parse_block({"elseif", "else", "end"})))
        otherwise: list[tuple[Any, ...]] = []
        if self.accept("else"):
            otherwise = self.parse_block({"end"})
        self.expect("end")
        return ("if", branches, otherwise)

    def parse_while(self) -> tuple[Any, ...]:
        self.expect("while")
        condition = self.parse_expression()
        self.expect("do")
        body = self.parse_block({"end"})
        self.expect("end")
        return ("while", condition, body)

    def parse_for(self) -> tuple[Any, ...]:
        self.expect("for")
        names = [self.expect_name().value]
        while self.accept(","):
            names.append(self.expect_name().value)
        if self.accept("="):
            start = self.parse_expression()
            self.expect(",")
            stop = self.parse_expression()
            step = None
            if self.accept(","):
                step = self.parse_expression()
            self.expect("do")
            body = self.parse_block({"end"})
            self.expect("end")
            if len(names) != 1:
                raise self.error("numeric for loops take one variable")
            return ("for_numeric", names[0], start, stop, step, body)
        if self.accept("in"):
            expressions = self.parse_expression_list()
            self.expect("do")
            body = self.parse_block({"end"})
            self.expect("end")
            return ("for_in", names, expressions, body)
        raise self.error("expected '=' or 'in' after for variable")

    def parse_function_target(self) -> tuple[Any, ...]:
        target: tuple[Any, ...] = ("var", self.expect_name().value)
        while self.accept("."):
            target = ("index", target, ("literal", self.expect_name().value))
        return target

    def parse_function_body(self) -> tuple[Any, ...]:
        self.expect("(")
        parameters: list[str] = []
        if not self.peek(")"):
            parameters.append(self.expect_name().value)
            while self.accept(","):
                parameters.append(self.expect_name().value)
        self.expect(")")
        body = self.parse_block({"end"})
        self.expect("end")
        return ("function", parameters, body)

    def parse_expression_list(self) -> list[tuple[Any, ...]]:
        expressions = [self.parse_expression()]
        while self.accept(","):
            expressions.append(self.parse_expression())
        return expressions

    def parse_expression(self, minimum_precedence: int = 0) -> tuple[Any, ...]:
        left = self.parse_unary_or_primary()
        while True:
            operator = self.current.value
            entry = self._precedence.get(operator)
            if entry is None or entry[0] < minimum_precedence:
                break
            precedence, right_associative = entry
            self.consume()
            right = self.parse_expression(precedence if right_associative else precedence + 1)
            left = ("binary", operator, left, right)
        return left

    def parse_unary_or_primary(self) -> tuple[Any, ...]:
        if self.current.value in {"not", "-", "#"}:
            operator = self.consume().value
            return ("unary", operator, self.parse_expression(8))
        expression = self.parse_primary()
        while True:
            if self.accept("."):
                expression = ("index", expression, ("literal", self.expect_name().value))
            elif self.accept("["):
                key = self.parse_expression()
                self.expect("]")
                expression = ("index", expression, key)
            elif self.peek("("):
                expression = ("call", expression, self.parse_call_arguments())
            elif self.accept(":"):
                method = self.expect_name().value
                expression = ("method_call", expression, method, self.parse_call_arguments())
            else:
                break
        return expression

    def parse_primary(self) -> tuple[Any, ...]:
        token = self.current
        if token.kind in {"number", "string"}:
            self.consume()
            return ("literal", token.value)
        if self.peek("function"):
            self.consume()
            return self.parse_function_body()
        if token.kind == "name":
            self.consume()
            if token.value == "true":
                return ("literal", True)
            if token.value == "false":
                return ("literal", False)
            if token.value == "nil":
                return ("literal", None)
            return ("var", token.value)
        if self.accept("("):
            expression = self.parse_expression()
            self.expect(")")
            return expression
        if self.peek("{"):
            return self.parse_table()
        raise self.error(f"expected expression, got {token.value!r}")

    def parse_call_arguments(self) -> list[tuple[Any, ...]]:
        self.expect("(")
        if self.peek(")"):
            self.consume()
            return []
        arguments = self.parse_expression_list()
        self.expect(")")
        return arguments

    def parse_table(self) -> tuple[Any, ...]:
        self.expect("{")
        entries: list[tuple[tuple[Any, ...] | None, tuple[Any, ...]]] = []
        next_array_index = 1
        while not self.peek("}"):
            key: tuple[Any, ...] | None = None
            if self.peek("["):
                self.consume()
                key = self.parse_expression()
                self.expect("]")
                self.expect("=")
            elif self.current.kind == "name" and self.peek("=", offset=1):
                key = ("literal", self.consume().value)
                self.expect("=")
            value = self.parse_expression()
            if key is None:
                key = ("literal", next_array_index)
                next_array_index += 1
            entries.append((key, value))
            if not self.accept(","):
                self.accept(";")
                if not self.peek("}"):
                    raise self.error("expected ',' or '}' in table")
            else:
                self.accept(";")
        self.expect("}")
        return ("table", entries)

    @staticmethod
    def is_assignable(expression: tuple[Any, ...]) -> bool:
        return expression[0] in {"var", "index"}

    def expect_name(self) -> Token:
        if self.current.kind != "name" or self.current.value in {
            "and",
            "break",
            "do",
            "else",
            "elseif",
            "end",
            "false",
            "for",
            "function",
            "if",
            "in",
            "local",
            "nil",
            "not",
            "or",
            "return",
            "then",
            "true",
            "while",
        }:
            raise self.error(f"expected name, got {self.current.value!r}")
        return self.consume()


class LuaTable:
    """A tiny Lua table implementation with predictable insertion order."""

    def __init__(self, values: Mapping[Any, Any] | None = None):
        self.values: dict[Any, Any] = dict(values or {})

    def __repr__(self) -> str:  # pragma: no cover - only used in diagnostics
        return f"LuaTable({self.values!r})"

    def __len__(self) -> int:
        numeric = [key for key in self.values if isinstance(key, int) and key > 0]
        return max(numeric, default=0)

    def __iter__(self) -> Iterator[tuple[Any, Any]]:
        return iter(self.values.items())


class LuaMultiReturn(tuple):
    """Marker used internally for functions returning more than one value."""


class LuaIterator:
    def __init__(self, iterator: Iterable[tuple[Any, ...]]):
        self._iterator = iter(iterator)

    def next(self) -> tuple[Any, ...] | None:
        try:
            return tuple(next(self._iterator))
        except StopIteration:
            return None


class Environment:
    def __init__(self, parent: Environment | None = None):
        self.values: dict[str, Any] = {}
        self.parent = parent

    def declare(self, name: str, value: Any = None) -> None:
        self.values[name] = value

    def get(self, name: str) -> Any:
        if name in self.values:
            return self.values[name]
        if self.parent is not None:
            return self.parent.get(name)
        return None

    def set(self, name: str, value: Any) -> None:
        if name in self.values:
            self.values[name] = value
        elif self.parent is not None and self.parent.has(name):
            self.parent.set(name, value)
        else:
            self.values[name] = value

    def has(self, name: str) -> bool:
        return name in self.values or (self.parent is not None and self.parent.has(name))


@dataclass
class ExecutionContext:
    max_instructions: int
    timeout_ms: int
    max_output: int
    instructions: int = 0

    def __post_init__(self) -> None:
        self.started = time.monotonic()
        self.output: list[str] = []
        self.events: list[dict[str, Any]] = []

    def tick(self) -> None:
        self.instructions += 1
        if self.instructions > self.max_instructions:
            raise ExecutionLimit(
                f"instruction limit exceeded ({self.max_instructions:,}); "
                "add a smaller loop or raise --instruction-limit"
            )
        if self.instructions % 64 == 0 and (time.monotonic() - self.started) * 1000 > self.timeout_ms:
            raise ExecutionLimit(f"execution timed out after {self.timeout_ms} ms")

    def write(self, value: str) -> None:
        current = sum(len(line) + 1 for line in self.output)
        if current + len(value) > self.max_output:
            raise ExecutionLimit(f"script output exceeded {self.max_output:,} bytes")
        self.output.append(value)


class HostAPI:
    """Safe, intentionally small bridge exposed to a script as ``host``.

    A host owns its state and may provide logger/event callbacks.  The bridge
    never exposes a Python object, socket, file, process, or module to Lua.
    """

    def __init__(
        self,
        name: str = "local-sandbox",
        host_id: str = "local",
        state: Mapping[str, Any] | None = None,
        logger: Callable[[str, str], None] | None = None,
        event_sink: Callable[[str, Any], None] | None = None,
    ):
        self.name_value = name
        self.id_value = host_id
        self.state = from_jsonable(dict(state or {"counter": 0}))
        if not isinstance(self.state, LuaTable):
            self.state = LuaTable()
        self.logger = logger
        self.event_sink = event_sink
        self.events: list[dict[str, Any]] = []
        self._started = time.monotonic()

    def _get(self, key: Any) -> Any:
        if not isinstance(key, str):
            raise LuaRuntimeError("host.get expects a string key")
        return self.state.values.get(key)

    def _set(self, key: Any, value: Any) -> bool:
        if not isinstance(key, str) or not key or len(key) > 80:
            raise LuaRuntimeError("host.set expects a non-empty key up to 80 characters")
        if key not in self.state.values and len(self.state.values) >= 256:
            raise LuaRuntimeError("host state key limit exceeded")
        was_present = key in self.state.values
        previous = self.state.values.get(key)
        checked = from_jsonable(to_jsonable(value))
        self.state.values[key] = checked
        # Keep a host from accumulating an unbounded JSON response through
        # many small writes while still allowing normal test fixtures to grow.
        if len(json.dumps(to_jsonable(self.state), separators=(",", ":"))) > 64_000:
            if was_present:
                self.state.values[key] = previous
            else:
                self.state.values.pop(key, None)
            raise LuaRuntimeError("host state size limit exceeded")
        return True

    def _patch(self, values: Any) -> int:
        if not isinstance(values, LuaTable):
            raise LuaRuntimeError("host.patch expects a table")
        original = to_jsonable(self.state)
        count = 0
        try:
            for key, value in values.values.items():
                if not isinstance(key, str):
                    raise LuaRuntimeError("host.patch keys must be strings")
                self._set(key, value)
                count += 1
        except LuaError:
            self.state = from_jsonable(original)
            raise
        return count

    def _emit(self, name: Any, payload: Any = None) -> bool:
        if not isinstance(name, str) or not name or len(name) > 80:
            raise LuaRuntimeError("host.emit expects a non-empty event name up to 80 characters")
        event = {"name": name, "payload": to_jsonable(payload), "at_ms": self.elapsed_ms()}
        self.events.append(event)
        if self.event_sink is not None:
            self.event_sink(name, payload)
        return True

    def _log(self, level: Any, message: Any) -> bool:
        level_text = str(level).lower()
        if level_text not in {"debug", "info", "warn", "error"}:
            raise LuaRuntimeError("host.log level must be debug, info, warn, or error")
        message_text = lua_tostring(message)
        if len(message_text) > 2_000:
            raise LuaRuntimeError("host.log message is too long")
        if self.logger is not None:
            self.logger(level_text, message_text)
        return True

    def snapshot(self) -> LuaTable:
        return from_jsonable(to_jsonable(self.state))

    def elapsed_ms(self) -> int:
        return int((time.monotonic() - self._started) * 1000)

    def api(self) -> LuaTable:
        return LuaTable(
            {
                "name": lambda: self.name_value,
                "id": lambda: self.id_value,
                "get": self._get,
                "set": self._set,
                "patch": self._patch,
                "emit": self._emit,
                "log": self._log,
                "snapshot": self.snapshot,
                "capabilities": lambda: from_jsonable(
                    ["state.read", "state.write", "events", "logging"]
                ),
                "time_ms": self.elapsed_ms,
            }
        )


class LuaFunction:
    def __init__(self, parameters: Sequence[str], body: Sequence[tuple[Any, ...]], closure: Environment):
        self.parameters = list(parameters)
        self.body = list(body)
        self.closure = closure

    def call(self, arguments: Sequence[Any], evaluator: Evaluator, context: ExecutionContext) -> Any:
        environment = Environment(self.closure)
        for index, name in enumerate(self.parameters):
            environment.declare(name, arguments[index] if index < len(arguments) else None)
        try:
            evaluator.eval_block(self.body, environment, context)
        except ReturnSignal as returned:
            return returned.value
        return None


class ReturnSignal(Exception):
    def __init__(self, value: Any):
        self.value = value


class BreakSignal(Exception):
    pass


def lua_truthy(value: Any) -> bool:
    return value is not None and value is not False


def lua_tostring(value: Any) -> str:
    if value is None:
        return "nil"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    if isinstance(value, LuaTable):
        return "table"
    if isinstance(value, LuaFunction) or callable(value):
        return "function"
    return str(value)


def lua_number(value: Any) -> int | float:
    if isinstance(value, bool):
        raise LuaRuntimeError("boolean is not a number")
    if isinstance(value, (int, float)):
        return value
    if isinstance(value, str):
        try:
            parsed = float(value.strip())
            return int(parsed) if parsed.is_integer() else parsed
        except ValueError as exc:
            raise LuaRuntimeError(f"cannot convert {value!r} to a number") from exc
    raise LuaRuntimeError(f"cannot convert {lua_tostring(value)} to a number")


def lua_type(value: Any) -> str:
    if value is None:
        return "nil"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, (int, float)):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, LuaTable):
        return "table"
    if isinstance(value, LuaFunction) or callable(value):
        return "function"
    return "userdata"


def _normalise_key(key: Any) -> Any:
    if isinstance(key, float) and key.is_integer():
        return int(key)
    if not isinstance(key, (str, int, float, bool)):
        raise LuaRuntimeError("table keys must be strings or numbers")
    return key


def get_index(container: Any, key: Any) -> Any:
    key = _normalise_key(key)
    if isinstance(container, LuaTable):
        return container.values.get(key)
    if isinstance(container, Mapping):
        return container.get(key)
    if isinstance(container, (list, tuple, str)):
        if not isinstance(key, int) or key < 1 or key > len(container):
            return None
        return container[key - 1]
    raise LuaRuntimeError(f"cannot index a {lua_type(container)} value")


def set_index(container: Any, key: Any, value: Any) -> None:
    key = _normalise_key(key)
    if isinstance(container, LuaTable):
        if len(container.values) >= 256 and key not in container.values:
            raise LuaRuntimeError("table size limit exceeded")
        container.values[key] = value
        return
    if isinstance(container, list):
        if not isinstance(key, int) or key < 1 or key > len(container) + 1:
            raise LuaRuntimeError("list index must be the next integer or an existing index")
        if key == len(container) + 1:
            container.append(value)
        else:
            container[key - 1] = value
        return
    raise LuaRuntimeError(f"cannot assign a {lua_type(container)} value")


def lua_len(value: Any) -> int:
    if isinstance(value, (str, list, tuple, LuaTable)):
        return len(value)
    raise LuaRuntimeError(f"attempt to get length of a {lua_type(value)} value")


def to_jsonable(value: Any, depth: int = 0) -> Any:
    """Convert a runtime value to bounded JSON-safe data."""
    if depth > 8:
        raise LuaRuntimeError("value nesting is too deep")
    if value is None or isinstance(value, (str, int, float, bool)):
        if isinstance(value, float) and (math.isnan(value) or math.isinf(value)):
            raise LuaRuntimeError("non-finite numbers are not supported")
        return value
    if isinstance(value, LuaMultiReturn):
        return [to_jsonable(item, depth + 1) for item in value]
    if isinstance(value, LuaTable):
        keys = list(value.values)
        numeric = [key for key in keys if isinstance(key, int) and key >= 1]
        if keys and len(numeric) == len(keys) and set(numeric) == set(range(1, len(keys) + 1)):
            return [to_jsonable(value.values[index], depth + 1) for index in range(1, len(keys) + 1)]
        result: dict[str, Any] = {}
        for key, item in value.values.items():
            result[str(key)] = to_jsonable(item, depth + 1)
        return result
    if isinstance(value, Mapping):
        return {str(key): to_jsonable(item, depth + 1) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [to_jsonable(item, depth + 1) for item in value]
    raise LuaRuntimeError(f"cannot serialize a {lua_type(value)} value")


def from_jsonable(value: Any, depth: int = 0) -> Any:
    if depth > 8:
        raise LuaRuntimeError("value nesting is too deep")
    if isinstance(value, dict):
        return LuaTable({key: from_jsonable(item, depth + 1) for key, item in value.items()})
    if isinstance(value, list):
        return LuaTable({index: from_jsonable(item, depth + 1) for index, item in enumerate(value, 1)})
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    raise LuaRuntimeError("only JSON-compatible values can cross the host boundary")


class Evaluator:
    def eval_block(
        self, statements: Sequence[tuple[Any, ...]], environment: Environment, context: ExecutionContext
    ) -> Any:
        result = None
        for statement in statements:
            context.tick()
            result = self.eval_statement(statement, environment, context)
        return result

    def eval_statement(
        self, statement: tuple[Any, ...], environment: Environment, context: ExecutionContext
    ) -> Any:
        kind = statement[0]
        if kind == "local":
            _, names, expressions = statement
            values = self.eval_expression_list(expressions, environment, context)
            for index, name in enumerate(names):
                environment.declare(name, values[index] if index < len(values) else None)
            return None
        if kind == "local_function":
            _, name, function_ast = statement
            environment.declare(name, self.eval_expression(function_ast, environment, context))
            return None
        if kind == "assign":
            _, targets, expressions = statement
            values = self.eval_expression_list(expressions, environment, context)
            for index, target in enumerate(targets):
                self.assign(target, values[index] if index < len(values) else None, environment, context)
            return None
        if kind == "expr":
            return self.eval_expression(statement[1], environment, context)
        if kind == "return":
            values = self.eval_expression_list(statement[1], environment, context)
            raise ReturnSignal(values[0] if len(values) == 1 else LuaMultiReturn(values))
        if kind == "break":
            raise BreakSignal()
        if kind == "block":
            return self.eval_block(statement[1], Environment(environment), context)
        if kind == "if":
            _, branches, otherwise = statement
            for condition, body in branches:
                if lua_truthy(self.eval_expression(condition, environment, context)):
                    return self.eval_block(body, Environment(environment), context)
            return self.eval_block(otherwise, Environment(environment), context) if otherwise else None
        if kind == "while":
            _, condition, body = statement
            result = None
            while lua_truthy(self.eval_expression(condition, environment, context)):
                context.tick()
                try:
                    result = self.eval_block(body, Environment(environment), context)
                except BreakSignal:
                    break
            return result
        if kind == "for_numeric":
            _, name, start_ast, stop_ast, step_ast, body = statement
            start = lua_number(self.eval_expression(start_ast, environment, context))
            stop = lua_number(self.eval_expression(stop_ast, environment, context))
            step = lua_number(self.eval_expression(step_ast, environment, context)) if step_ast else 1
            if step == 0:
                raise LuaRuntimeError("numeric for step cannot be zero")
            current = start
            result = None
            condition = (lambda value: value <= stop) if step > 0 else (lambda value: value >= stop)
            while condition(current):
                context.tick()
                loop_environment = Environment(environment)
                loop_environment.declare(name, current)
                try:
                    result = self.eval_block(body, loop_environment, context)
                except BreakSignal:
                    break
                current += step
            return result
        if kind == "for_in":
            _, names, expression_asts, body = statement
            values = self.eval_expression_list(expression_asts, environment, context)
            iterator = values[0] if values else None
            if not isinstance(iterator, LuaIterator):
                iterator = LuaIterator(self.iter_pairs(iterator))
            result = None
            while True:
                context.tick()
                item = iterator.next()
                if item is None:
                    break
                loop_environment = Environment(environment)
                for index, name in enumerate(names):
                    loop_environment.declare(name, item[index] if index < len(item) else None)
                try:
                    result = self.eval_block(body, loop_environment, context)
                except BreakSignal:
                    break
            return result
        raise LuaRuntimeError(f"unsupported statement {kind}")

    def eval_expression_list(
        self, expressions: Sequence[tuple[Any, ...]], environment: Environment, context: ExecutionContext
    ) -> list[Any]:
        values: list[Any] = []
        for expression in expressions:
            value = self.eval_expression(expression, environment, context)
            if isinstance(value, LuaMultiReturn):
                values.extend(value)
            else:
                values.append(value)
        return values

    def eval_expression(
        self, expression: tuple[Any, ...], environment: Environment, context: ExecutionContext
    ) -> Any:
        context.tick()
        kind = expression[0]
        if kind == "literal":
            return expression[1]
        if kind == "var":
            return environment.get(expression[1])
        if kind == "table":
            table = LuaTable()
            for key_ast, value_ast in expression[1]:
                key = self.eval_expression(key_ast, environment, context)
                value = self.eval_expression(value_ast, environment, context)
                set_index(table, key, value)
            return table
        if kind == "function":
            return LuaFunction(expression[1], expression[2], environment)
        if kind == "index":
            container = self.eval_expression(expression[1], environment, context)
            key = self.eval_expression(expression[2], environment, context)
            return get_index(container, key)
        if kind == "call":
            function = self.eval_expression(expression[1], environment, context)
            arguments = self.eval_expression_list(expression[2], environment, context)
            return self.call_value(function, arguments, context)
        if kind == "method_call":
            container = self.eval_expression(expression[1], environment, context)
            function = get_index(container, expression[2])
            arguments = self.eval_expression_list(expression[3], environment, context)
            return self.call_value(function, [container, *arguments], context)
        if kind == "unary":
            operator = expression[1]
            value = self.eval_expression(expression[2], environment, context)
            if operator == "not":
                return not lua_truthy(value)
            if operator == "-":
                return -lua_number(value)
            if operator == "#":
                return lua_len(value)
        if kind == "binary":
            operator = expression[1]
            left = self.eval_expression(expression[2], environment, context)
            if operator == "and":
                return self.eval_expression(expression[3], environment, context) if lua_truthy(left) else left
            if operator == "or":
                return left if lua_truthy(left) else self.eval_expression(expression[3], environment, context)
            right = self.eval_expression(expression[3], environment, context)
            return self.binary(operator, left, right)
        raise LuaRuntimeError(f"unsupported expression {kind}")

    def assign(
        self, target: tuple[Any, ...], value: Any, environment: Environment, context: ExecutionContext
    ) -> None:
        if target[0] == "var":
            environment.set(target[1], value)
            return
        if target[0] == "index":
            container = self.eval_expression(target[1], environment, context)
            key = self.eval_expression(target[2], environment, context)
            set_index(container, key, value)
            return
        raise LuaRuntimeError("invalid assignment target")

    def call_value(self, function: Any, arguments: Sequence[Any], context: ExecutionContext) -> Any:
        context.tick()
        try:
            if isinstance(function, LuaFunction):
                return function.call(arguments, self, context)
            if callable(function):
                return function(*arguments)
        except LuaError:
            raise
        except Exception as exc:  # do not expose host implementation tracebacks to scripts
            raise LuaRuntimeError(str(exc) or "host function failed") from exc
        raise LuaRuntimeError(f"attempt to call a {lua_type(function)} value")

    @staticmethod
    def binary(operator: str, left: Any, right: Any) -> Any:
        if operator == "..":
            return lua_tostring(left) + lua_tostring(right)
        if operator in {"+", "-", "*", "/", "%", "^"}:
            left_number, right_number = lua_number(left), lua_number(right)
            if operator == "+":
                return left_number + right_number
            if operator == "-":
                return left_number - right_number
            if operator == "*":
                return left_number * right_number
            if operator == "/":
                if right_number == 0:
                    raise LuaRuntimeError("division by zero")
                return left_number / right_number
            if operator == "%":
                return left_number % right_number
            return left_number**right_number
        if operator in {"==", "~=", "<", ">", "<=", ">="}:
            if operator == "==":
                return left == right
            if operator == "~=":
                return left != right
            try:
                if operator == "<":
                    return left < right
                if operator == ">":
                    return left > right
                if operator == "<=":
                    return left <= right
                return left >= right
            except TypeError as exc:
                raise LuaRuntimeError("values cannot be compared") from exc
        raise LuaRuntimeError(f"unsupported operator {operator}")

    @staticmethod
    def iter_pairs(value: Any) -> Iterable[tuple[Any, ...]]:
        if isinstance(value, LuaTable):
            return ((key, item) for key, item in value.values.items())
        if isinstance(value, Mapping):
            return ((key, item) for key, item in value.items())
        if isinstance(value, (list, tuple, str)):
            return ((index, item) for index, item in enumerate(value, 1))
        if value is None:
            return iter(())
        raise LuaRuntimeError(f"cannot iterate a {lua_type(value)} value")


def _table_insert(table: Any, value: Any, position: Any = None) -> bool:
    if not isinstance(table, LuaTable):
        raise LuaRuntimeError("table.insert expects a table")
    values = table.values
    if position is None:
        position_number = len(table) + 1
    else:
        position_number = int(lua_number(position))
    if position_number < 1 or position_number > len(table) + 1:
        raise LuaRuntimeError("table.insert position is out of range")
    for index in range(len(table) + 1, position_number, -1):
        values[index] = values.get(index - 1)
    values[position_number] = value
    return True


def _table_remove(table: Any, position: Any = None) -> Any:
    if not isinstance(table, LuaTable):
        raise LuaRuntimeError("table.remove expects a table")
    length = len(table)
    if length == 0:
        return None
    index = length if position is None else int(lua_number(position))
    if index < 1 or index > length:
        raise LuaRuntimeError("table.remove position is out of range")
    result = table.values.get(index)
    for current in range(index, length):
        table.values[current] = table.values.get(current + 1)
    table.values.pop(length, None)
    return result


def _table_concat(table: Any, separator: Any = "", start: Any = 1, stop: Any = None) -> str:
    if not isinstance(table, LuaTable):
        raise LuaRuntimeError("table.concat expects a table")
    first = int(lua_number(start))
    last = len(table) if stop is None else int(lua_number(stop))
    return str(separator).join(lua_tostring(table.values.get(index)) for index in range(first, last + 1))


def _string_sub(value: Any, start: Any, stop: Any = None) -> str:
    text = str(value)
    first = int(lua_number(start))
    last = len(text) if stop is None else int(lua_number(stop))
    if first < 0:
        first = len(text) + first + 1
    if last < 0:
        last = len(text) + last + 1
    return text[max(first - 1, 0) : last]


def _json_encode(value: Any) -> str:
    return json.dumps(to_jsonable(value), separators=(",", ":"), ensure_ascii=False)


def _json_decode(value: Any) -> Any:
    if not isinstance(value, str) or len(value) > 128_000:
        raise LuaRuntimeError("json.decode expects a string up to 128 KB")
    try:
        return from_jsonable(json.loads(value))
    except (json.JSONDecodeError, LuaError) as exc:
        raise LuaRuntimeError(f"invalid JSON: {exc}") from exc


class Sandbox:
    """Compile and execute scripts with instruction, time, and output limits."""

    MAX_SOURCE = 128_000
    MAX_INSTRUCTIONS = 1_000_000
    MAX_TIMEOUT_MS = 30_000
    MAX_OUTPUT = 64_000

    def __init__(
        self,
        host: HostAPI | None = None,
        *,
        max_instructions: int = 100_000,
        timeout_ms: int = 2_000,
        max_output: int = 16_000,
    ):
        if not 1 <= max_instructions <= self.MAX_INSTRUCTIONS:
            raise ValueError(f"max_instructions must be between 1 and {self.MAX_INSTRUCTIONS}")
        if not 1 <= timeout_ms <= self.MAX_TIMEOUT_MS:
            raise ValueError(f"timeout_ms must be between 1 and {self.MAX_TIMEOUT_MS}")
        if not 1 <= max_output <= self.MAX_OUTPUT:
            raise ValueError(f"max_output must be between 1 and {self.MAX_OUTPUT}")
        self.host = host or HostAPI()
        self.max_instructions = max_instructions
        self.timeout_ms = timeout_ms
        self.max_output = max_output

    def compile(self, source: str) -> list[tuple[Any, ...]]:
        if not isinstance(source, str):
            raise LuaSyntaxError("script must be text")
        if len(source.encode("utf-8")) > self.MAX_SOURCE:
            raise LuaSyntaxError(f"script is larger than {self.MAX_SOURCE:,} bytes")
        return Parser(Lexer(source).tokenize()).parse()

    def validate(self, source: str) -> None:
        self.compile(source)

    def execute(self, source: str) -> dict[str, Any]:
        started = time.monotonic()
        context = ExecutionContext(self.max_instructions, self.timeout_ms, self.max_output)
        # Events belong to this execution, not to every execution ever sent to
        # a long-lived host.  The host clears its event queue too, but doing it
        # here keeps Sandbox useful on its own and prevents duplicate results.
        event_start = len(self.host.events)
        try:
            program = self.compile(source)
            environment = self._global_environment(context)
            try:
                result = Evaluator().eval_block(program, environment, context)
            except ReturnSignal as returned:
                result = returned.value
            result_json = to_jsonable(result)
            status = "ok"
            error = None
        except LuaError as exc:
            result_json = None
            status = "error"
            error = str(exc)
        except Exception as exc:  # keep the protocol response stable even for a host callback bug
            result_json = None
            status = "error"
            error = f"internal runtime error: {exc}"
        return {
            "status": status,
            "result": result_json,
            "output": list(context.output),
            "events": list(self.host.events[event_start:]),
            "instructions": context.instructions,
            "duration_ms": round((time.monotonic() - started) * 1000, 2),
            "error": error,
        }

    def _global_environment(self, context: ExecutionContext) -> Environment:
        random_source = random.Random(0)

        def print_function(*values: Any) -> None:
            context.write("\t".join(lua_tostring(value) for value in values))

        def warn_function(*values: Any) -> None:
            context.write("WARN: " + "\t".join(lua_tostring(value) for value in values))

        def assert_function(condition: Any, message: Any = "assertion failed") -> Any:
            if not lua_truthy(condition):
                raise LuaRuntimeError(lua_tostring(message))
            return condition

        def error_function(message: Any = "script error") -> None:
            raise LuaRuntimeError(lua_tostring(message))

        def pairs_function(value: Any) -> LuaIterator:
            return LuaIterator(Evaluator.iter_pairs(value))

        def ipairs_function(value: Any) -> LuaIterator:
            if isinstance(value, LuaTable):
                return LuaIterator(
                    (index, value.values[index])
                    for index in range(1, len(value) + 1)
                    if index in value.values
                )
            if isinstance(value, (list, tuple, str)):
                return LuaIterator((index, item) for index, item in enumerate(value, 1))
            raise LuaRuntimeError("ipairs expects a table or sequence")

        def next_function(value: Any, key: Any = None) -> LuaMultiReturn | None:
            entries = list(Evaluator.iter_pairs(value))
            if key is None:
                return LuaMultiReturn(entries[0]) if entries else None
            for index, (entry_key, entry_value) in enumerate(entries):
                if entry_key == key and index + 1 < len(entries):
                    return LuaMultiReturn(entries[index + 1])
            return None

        def pcall_function(function: Any, *arguments: Any) -> LuaMultiReturn:
            try:
                return LuaMultiReturn((True, Evaluator().call_value(function, arguments, context)))
            except LuaError as exc:
                return LuaMultiReturn((False, str(exc)))

        math_table = LuaTable(
            {
                "abs": abs,
                "ceil": math.ceil,
                "floor": math.floor,
                "max": lambda *values: max(lua_number(value) for value in values),
                "min": lambda *values: min(lua_number(value) for value in values),
                "sqrt": math.sqrt,
                "random": lambda: random_source.random(),
                "randomseed": lambda seed=0: random_source.seed(int(lua_number(seed))),
                "pi": math.pi,
            }
        )
        string_table = LuaTable(
            {
                "len": lambda value: len(str(value)),
                "lower": lambda value: str(value).lower(),
                "upper": lambda value: str(value).upper(),
                "sub": _string_sub,
                "find": lambda value, needle: (str(value).find(str(needle)) + 1) or None,
                "format": lambda template, *values: str(template).format(*values),
            }
        )
        table_table = LuaTable(
            {"insert": _table_insert, "remove": _table_remove, "concat": _table_concat}
        )
        json_table = LuaTable({"encode": _json_encode, "decode": _json_decode})
        environment = Environment()
        for name, value in {
            "_VERSION": "Lua Injection subset 1.0",
            "print": print_function,
            "warn": warn_function,
            "type": lua_type,
            "tostring": lua_tostring,
            "tonumber": lua_number,
            "assert": assert_function,
            "error": error_function,
            "pairs": pairs_function,
            "ipairs": ipairs_function,
            "next": next_function,
            "pcall": pcall_function,
            "math": math_table,
            "string": string_table,
            "table": table_table,
            "json": json_table,
            "host": self.host.api(),
        }.items():
            environment.declare(name, value)
        return environment


__all__ = [
    "ExecutionLimit",
    "HostAPI",
    "LuaError",
    "LuaRuntimeError",
    "LuaSyntaxError",
    "LuaTable",
    "Sandbox",
    "from_jsonable",
    "to_jsonable",
]
