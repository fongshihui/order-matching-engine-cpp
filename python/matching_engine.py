import struct
import subprocess
import socket
import sys
import threading
import time
from pathlib import Path
from typing import Any

Command = dict[str, Any]
ExecutionReport = dict[str, Any]

THIS_DIR = Path(__file__).resolve().parent
ROOT_DIR = THIS_DIR.parent


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


def _candidate_proto_modules() -> list[Path]:
    return [
        build_dir / "generated" / "python"
        for build_dir in sorted(ROOT_DIR.glob("build*"))
    ]


def _find_server_binary() -> Path:
    for path in _candidate_server_paths():
        if path.exists():
            return path
    raise FileNotFoundError(
        f"matching_engine_server not found. Build the project first from {ROOT_DIR}"
    )


def _load_proto_bindings() -> Any:
    # Use the generated Python protobuf module directly now that the local
    # protobuf runtime has been upgraded to a compatible version.
    for directory in _candidate_proto_modules():
        module_path = directory / "matching_engine_pb2.py"
        if not module_path.exists():
            continue
        if str(directory) not in sys.path:
            sys.path.insert(0, str(directory))
        import matching_engine_pb2

        return matching_engine_pb2
    raise FileNotFoundError(
        "matching_engine_pb2.py not found. Build the project first so protoc can generate it."
    )


def _read_exact_socket(sock: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = sock.recv(size - len(chunks))
        if not chunk:
            break
        chunks.extend(chunk)
    return bytes(chunks)


def _reserve_local_port(host: str) -> tuple[int, socket.socket]:
    # Reserve an ephemeral port before spawning the server so the child can be
    # launched with a concrete port number and the parent can connect to it.
    reserved = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    reserved.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    reserved.bind((host, 0))
    return int(reserved.getsockname()[1]), reserved


def _connect_with_retry(host: str, port: int, timeout_s: float = 5.0) -> socket.socket:
    deadline = time.monotonic() + timeout_s
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        try:
            connection = socket.create_connection((host, port), timeout=0.5)
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            return connection
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(
        f"failed to connect to matching engine server on {host}:{port}: {last_error}"
    )


class MatchingEngine:
    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int | None = None,
        start_server: bool = True,
    ) -> None:
        self._lock = threading.Lock()
        self._closed = False
        self._proto = _load_proto_bindings()
        self._host = host
        self._owns_server = start_server
        self._process: subprocess.Popen[bytes] | None = None

        reserved_socket: socket.socket | None = None
        if port is None:
            if not start_server:
                raise ValueError("port must be provided when start_server is False")
            port, reserved_socket = _reserve_local_port(host)
        self._port = port

        try:
            if start_server:
                # Default mode: Python owns the C++ TCP server lifecycle and
                # connects to it over localhost once the process is listening.
                self._process = subprocess.Popen(
                    [str(_find_server_binary()), "--port", str(self._port)],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE,
                    cwd=ROOT_DIR,
                )
            if reserved_socket is not None:
                reserved_socket.close()
                reserved_socket = None
            self._socket = _connect_with_retry(self._host, self._port)
        except Exception:
            if reserved_socket is not None:
                reserved_socket.close()
            if self._process is not None:
                self._process.kill()
                self._process.wait(timeout=1)
            raise

    def process(self, command: Command) -> ExecutionReport:
        request = self._proto.RequestEnvelope()
        request.process.CopyFrom(self._command_to_proto(command))
        response = self._round_trip(request)
        if response.WhichOneof("payload") != "report":
            raise RuntimeError(
                f"unexpected server response: {response.WhichOneof('payload')}"
            )
        return self._report_to_dict(response.report)

    def best_bid(self) -> int | None:
        return self._request_optional_price("best_bid")

    def best_ask(self) -> int | None:
        return self._request_optional_price("best_ask")

    def depth(self, side: str) -> list[dict[str, int]]:
        request = self._proto.RequestEnvelope()
        request.depth.side = self._side_to_proto(side)
        response = self._round_trip(request)
        if response.WhichOneof("payload") != "depth":
            raise RuntimeError(
                f"unexpected server response: {response.WhichOneof('payload')}"
            )
        # Depth returns aggregated price levels rather than individual orders.
        return [
            {"price": level.price, "qty": level.qty} for level in response.depth.levels
        ]

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            if self._owns_server:
                # If this client started the server, ask it to stop cleanly so
                # the listening socket is released before falling back to kill().
                try:
                    request = self._proto.RequestEnvelope()
                    request.quit.SetInParent()
                    response = self._round_trip_locked(request)
                    if response.WhichOneof("payload") != "quit":
                        raise RuntimeError(
                            f"unexpected server response: {response.WhichOneof('payload')}"
                        )
                except Exception:
                    if self._process is not None and self._process.poll() is None:
                        self._process.kill()
            self._socket.close()
            self._closed = True
            if self._process is not None:
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

    def _request_optional_price(self, field_name: str) -> int | None:
        request = self._proto.RequestEnvelope()
        getattr(request, field_name).SetInParent()
        response = self._round_trip(request)
        if response.WhichOneof("payload") != field_name:
            raise RuntimeError(
                f"unexpected server response: {response.WhichOneof('payload')}"
            )
        message = getattr(response, field_name)
        return message.price if message.HasField("price") else None

    def _command_to_proto(self, command: Command) -> Any:
        process = self._proto.ProcessRequest()
        command_name = str(command["command"]).upper()

        # Convert the public Python dict API into the strongly typed protobuf
        # request expected by the C++ server.
        if command_name in {"NEW_LIMIT", "NEW_MARKET", "NEW"}:
            message = process.new_order
            message.order_id = int(command["order_id"])
            message.user_id = int(command["user_id"])
            message.side = self._side_to_proto(str(command["side"]))
            if command_name == "NEW_LIMIT":
                message.type = self._proto.ORDER_TYPE_LIMIT
            elif command_name == "NEW_MARKET":
                message.type = self._proto.ORDER_TYPE_MARKET
            else:
                message.type = self._order_type_to_proto(str(command["type"]))
            if "price" in command and command["price"] is not None:
                message.price = int(command["price"])
            message.qty = int(command["qty"])
            flags = dict(command.get("flags", {}) or {})
            message.flags.ioc = bool(flags.get("ioc", False))
            message.flags.fok = bool(flags.get("fok", False))
            message.flags.post_only = bool(flags.get("post_only", False))
            message.flags.stp = bool(flags.get("stp", False))
            return process

        if command_name == "CANCEL":
            message = process.cancel
            message.order_id = int(command["order_id"])
            message.user_id = int(command["user_id"])
            return process

        if command_name == "MODIFY":
            message = process.modify
            message.order_id = int(command["order_id"])
            if "price" in command and command["price"] is not None:
                message.price = int(command["price"])
            if "qty" in command and command["qty"] is not None:
                message.qty = int(command["qty"])
            return process

        raise RuntimeError(f"unknown command: {command_name}")

    def _report_to_dict(self, report: Any) -> ExecutionReport:
        # Preserve the original Python-facing dictionary shape so the transport
        # change does not force callers to rewrite their application code.
        return {
            "command": self._report_command_to_string(report.command),
            "order_id": int(report.order_id),
            "rejected": bool(report.rejected),
            "reject_reason": report.reject_reason,
            "reject_message": report.reject_message,
            "canceled": bool(report.canceled),
            "fills": [
                {
                    "maker_order_id": int(fill.maker_order_id),
                    "maker_user_id": int(fill.maker_user_id),
                    "taker_order_id": int(fill.taker_order_id),
                    "taker_user_id": int(fill.taker_user_id),
                    "side": self._side_to_string(fill.side),
                    "price": int(fill.price),
                    "qty": int(fill.qty),
                }
                for fill in report.fills
            ],
            "filled_qty": int(report.filled_qty),
        }

    def _round_trip(self, request: Any) -> Any:
        with self._lock:
            return self._round_trip_locked(request)

    def _round_trip_locked(self, request: Any) -> Any:
        if self._closed:
            raise RuntimeError("matching engine client is closed")
        payload = request.SerializeToString()
        frame = struct.pack(">I", len(payload)) + payload
        # The TCP transport uses a 4-byte big-endian length prefix followed by
        # the serialized protobuf payload, matching the C++ server framing.
        self._socket.sendall(frame)

        response = self._read_response_locked()
        if response.WhichOneof("payload") == "error":
            raise RuntimeError(response.error.message)
        return response

    def _read_response_locked(self) -> Any:
        size_bytes = _read_exact_socket(self._socket, 4)
        if len(size_bytes) != 4:
            raise RuntimeError(self._server_exit_message())
        (size,) = struct.unpack(">I", size_bytes)
        payload = _read_exact_socket(self._socket, size)
        if len(payload) != size:
            raise RuntimeError(self._server_exit_message())
        response = self._proto.ResponseEnvelope()
        if not response.ParseFromString(payload):
            raise RuntimeError("failed to parse protobuf response")
        return response

    def _server_exit_message(self) -> str:
        stderr_output = b""
        if self._process is not None and self._process.stderr is not None:
            stderr_output = self._process.stderr.read().strip()
        message = "matching engine server stopped unexpectedly"
        if stderr_output:
            message += f": {stderr_output.decode('utf-8', errors='replace')}"
        return message

    def _side_to_proto(self, side: str) -> int:
        side_upper = side.upper()
        if side_upper == "BUY":
            return self._proto.SIDE_BUY
        if side_upper == "SELL":
            return self._proto.SIDE_SELL
        raise RuntimeError(f"invalid side: {side}")

    def _order_type_to_proto(self, order_type: str) -> int:
        type_upper = order_type.upper()
        if type_upper == "LIMIT":
            return self._proto.ORDER_TYPE_LIMIT
        if type_upper == "MARKET":
            return self._proto.ORDER_TYPE_MARKET
        raise RuntimeError(f"invalid order type: {order_type}")

    def _side_to_string(self, side: int) -> str:
        if side == self._proto.SIDE_BUY:
            return "BUY"
        if side == self._proto.SIDE_SELL:
            return "SELL"
        raise RuntimeError(f"invalid protobuf side: {side}")

    def _report_command_to_string(self, command: int) -> str:
        if command == self._proto.REPORT_COMMAND_NEW_LIMIT:
            return "NEW_LIMIT"
        if command == self._proto.REPORT_COMMAND_NEW_MARKET:
            return "NEW_MARKET"
        if command == self._proto.REPORT_COMMAND_CANCEL:
            return "CANCEL"
        if command == self._proto.REPORT_COMMAND_MODIFY:
            return "MODIFY"
        return "UNKNOWN"
