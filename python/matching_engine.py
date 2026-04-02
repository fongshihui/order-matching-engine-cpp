import subprocess
import threading
from pathlib import Path
from typing import Any


Command = dict[str, Any]
ExecutionReport = dict[str, Any]

THIS_DIR = Path(__file__).resolve().parent
ROOT_DIR = THIS_DIR.parent


def _hex_decode(value: str) -> str:
    if not value:
        return ""
    return bytes.fromhex(value).decode("utf-8")


def _parse_fields(line: str) -> tuple[str, dict[str, str]]:
    parts = line.rstrip("\n").split("\t")
    prefix = parts[0]
    fields: dict[str, str] = {}
    for token in parts[1:]:
        if "=" not in token:
            raise RuntimeError(f"invalid server response field: {token}")
        key, value = token.split("=", 1)
        fields[key] = value
    return prefix, fields


def _candidate_server_paths() -> list[Path]:
    return [
        THIS_DIR / "matching_engine_server",
        THIS_DIR / "matching_engine_server.exe",
        ROOT_DIR / "build" / "matching_engine_server",
        ROOT_DIR / "build" / "Release" / "matching_engine_server.exe",
        ROOT_DIR / "build" / "Release" / "matching_engine_server",
        ROOT_DIR / "build" / "Debug" / "matching_engine_server.exe",
        ROOT_DIR / "build" / "Debug" / "matching_engine_server",
    ]


def _find_server_binary() -> Path:
    for path in _candidate_server_paths():
        if path.exists():
            return path
    raise FileNotFoundError(
        f"matching_engine_server not found. Build the project first from {ROOT_DIR}"
    )


class MatchingEngine:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._closed = False
        self._process = subprocess.Popen(
            [str(_find_server_binary())],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            cwd=ROOT_DIR,
        )

    def process(self, command: Command) -> ExecutionReport:
        flags = dict(command.get("flags", {}) or {})
        fields = [
            "PROCESS",
            f"command={command['command']}",
            f"order_id={command['order_id']}",
        ]
        if "user_id" in command:
            fields.append(f"user_id={command['user_id']}")
        if "side" in command:
            fields.append(f"side={command['side']}")
        if "type" in command:
            fields.append(f"type={command['type']}")
        if "price" in command and command["price"] is not None:
            fields.append(f"price={command['price']}")
        if "qty" in command and command["qty"] is not None:
            fields.append(f"qty={command['qty']}")
        fields.extend(
            [
                f"ioc={1 if flags.get('ioc', False) else 0}",
                f"fok={1 if flags.get('fok', False) else 0}",
                f"post_only={1 if flags.get('post_only', False) else 0}",
                f"stp={1 if flags.get('stp', False) else 0}",
            ]
        )
        with self._lock:
            self._send_line("\t".join(fields))
            prefix, report_fields = self._read_response_line()
            if prefix != "REPORT":
                raise RuntimeError(f"unexpected server response: {prefix}")

            fills: list[dict[str, Any]] = []
            while True:
                line = self._read_line()
                fill_prefix, fill_fields = _parse_fields(line)
                if fill_prefix == "END":
                    break
                if fill_prefix != "FILL":
                    raise RuntimeError(f"unexpected report payload: {fill_prefix}")
                fills.append(
                    {
                        "maker_order_id": int(fill_fields["maker_order_id"]),
                        "maker_user_id": int(fill_fields["maker_user_id"]),
                        "taker_order_id": int(fill_fields["taker_order_id"]),
                        "taker_user_id": int(fill_fields["taker_user_id"]),
                        "side": fill_fields["side"],
                        "price": int(fill_fields["price"]),
                        "qty": int(fill_fields["qty"]),
                    }
                )

            return {
                "command": report_fields["command"],
                "order_id": int(report_fields["order_id"]),
                "rejected": report_fields["rejected"] == "1",
                "reject_reason": _hex_decode(report_fields["reject_reason_hex"]),
                "reject_message": _hex_decode(report_fields["reject_message_hex"]),
                "canceled": report_fields["canceled"] == "1",
                "fills": fills,
                "filled_qty": int(report_fields["filled_qty"]),
            }

    def best_bid(self) -> int | None:
        return self._request_optional_value("BEST_BID")

    def best_ask(self) -> int | None:
        return self._request_optional_value("BEST_ASK")

    def depth(self, side: str) -> list[dict[str, int]]:
        with self._lock:
            self._send_line(f"DEPTH\tside={side}")
            prefix, _ = self._read_response_line()
            if prefix != "DEPTH":
                raise RuntimeError(f"unexpected server response: {prefix}")

            levels: list[dict[str, int]] = []
            while True:
                line = self._read_line()
                level_prefix, fields = _parse_fields(line)
                if level_prefix == "END":
                    return levels
                if level_prefix != "LEVEL":
                    raise RuntimeError(f"unexpected depth payload: {level_prefix}")
                levels.append(
                    {
                        "price": int(fields["price"]),
                        "qty": int(fields["qty"]),
                    }
                )

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            if self._process.stdin is not None and self._process.poll() is None:
                try:
                    self._send_line("QUIT")
                    line = self._read_line()
                    prefix, _ = _parse_fields(line)
                    if prefix != "BYE":
                        raise RuntimeError(f"unexpected server response: {prefix}")
                except Exception:
                    self._process.kill()
            self._closed = True
            self._process.wait(timeout=1)

    def __enter__(self) -> "MatchingEngine":
        return self

    def __exit__(self, *_: Any) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def _request_optional_value(self, operation: str) -> int | None:
        with self._lock:
            self._send_line(operation)
            prefix, fields = self._read_response_line()
            if prefix != "VALUE":
                raise RuntimeError(f"unexpected server response: {prefix}")
            value = fields["value"]
            return None if value == "NONE" else int(value)

    def _send_line(self, line: str) -> None:
        if self._closed:
            raise RuntimeError("matching engine client is closed")
        if self._process.stdin is None:
            raise RuntimeError("matching engine server stdin is unavailable")
        self._process.stdin.write(f"{line}\n")
        self._process.stdin.flush()

    def _read_response_line(self) -> tuple[str, dict[str, str]]:
        line = self._read_line()
        prefix, fields = _parse_fields(line)
        if prefix == "ERROR":
            raise RuntimeError(_hex_decode(fields["message_hex"]))
        return prefix, fields

    def _read_line(self) -> str:
        if self._process.stdout is None:
            raise RuntimeError("matching engine server stdout is unavailable")
        line = self._process.stdout.readline()
        if line:
            return line
        stderr_output = ""
        if self._process.stderr is not None:
            stderr_output = self._process.stderr.read().strip()
        raise RuntimeError(
            "matching engine server stopped unexpectedly"
            + (f": {stderr_output}" if stderr_output else "")
        )
