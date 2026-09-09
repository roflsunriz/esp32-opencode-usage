from __future__ import annotations

import codecs
import json
import queue
import signal
import sys
import threading
import time
from dataclasses import dataclass
from types import FrameType
from typing import BinaryIO

import serial
from serial.tools import list_ports


BAUD_RATE: int = 115200
READ_TIMEOUT_SECONDS: float = 0.1
WRITE_TIMEOUT_SECONDS: float = 3.0
MAXIMUM_INPUT_BYTES: int = 16_384
ERROR_MESSAGE: str = "serial communication failed"

output_lock: threading.Lock = threading.Lock()
stop_event: threading.Event = threading.Event()
failure_event: threading.Event = threading.Event()


@dataclass(frozen=True)
class WriteCommand:
    identifier: int
    data: str


@dataclass(frozen=True)
class CloseCommand:
    pass


Command = WriteCommand | CloseCommand
InputLine = tuple[bytes, bool]


def write_stdout_line(line: str) -> None:
    with output_lock:
        try:
            sys.stdout.buffer.write((line + "\n").encode("utf-8"))
            sys.stdout.buffer.flush()
        except (BrokenPipeError, OSError):
            stop_event.set()


def emit(event: dict[str, object]) -> None:
    write_stdout_line(json.dumps(event, ensure_ascii=False, separators=(",", ":")))


def emit_error() -> None:
    emit({"type": "error", "message": ERROR_MESSAGE})


def emit_close() -> None:
    emit({"type": "close"})


def write_list() -> int:
    try:
        result: list[dict[str, str]] = []
        for info in list_ports.comports():
            path: str = str(info.device)
            label_parts: list[str] = [path]
            if info.manufacturer:
                label_parts.append(str(info.manufacturer))
            result.append({"path": path, "label": " · ".join(label_parts)})
        write_stdout_line(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
        return 0
    except Exception:
        write_stdout_line("[]")
        return 1


def valid_port_path(value: str) -> bool:
    return (
        0 < len(value) <= 260
        and "\x00" not in value
        and "\r" not in value
        and "\n" not in value
    )


def parse_command(line: bytes) -> Command | None:
    try:
        value: object = json.loads(line.rstrip(b"\r\n").decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(value, dict):
        return None
    command_type: object = value.get("type")
    if command_type == "close":
        return CloseCommand()
    identifier: object = value.get("id")
    data: object = value.get("data")
    if (
        command_type != "write"
        or not isinstance(identifier, int)
        or isinstance(identifier, bool)
        or identifier < -9_007_199_254_740_991
        or identifier > 9_007_199_254_740_991
        or not isinstance(data, str)
    ):
        return None
    return WriteCommand(identifier=identifier, data=data)


def read_input_line(stream: BinaryIO) -> InputLine | None:
    line: bytes = stream.readline(MAXIMUM_INPUT_BYTES + 1)
    if not line:
        return None
    if len(line) <= MAXIMUM_INPUT_BYTES:
        return (line, False)

    while line and not line.endswith(b"\n"):
        line = stream.readline(4096)
    return (b"", True)


def stdin_loop(destination: queue.Queue[InputLine | None]) -> None:
    while not stop_event.is_set():
        line: InputLine | None = read_input_line(sys.stdin.buffer)
        destination.put(line)
        if line is None:
            return


def receive_loop(serial_port: serial.Serial) -> None:
    decoder: codecs.IncrementalDecoder = codecs.getincrementaldecoder("utf-8")(
        "replace"
    )
    try:
        while not stop_event.is_set():
            try:
                chunk: bytes = serial_port.read(1024)
            except (serial.SerialException, OSError):
                if not stop_event.is_set():
                    failure_event.set()
                    emit_error()
                    stop_event.set()
                return
            if not chunk:
                continue
            data: str = decoder.decode(chunk, final=False)
            if data:
                emit({"type": "data", "data": data})
    finally:
        trailing: str = decoder.decode(b"", final=True)
        if trailing:
            emit({"type": "data", "data": trailing})


def write_all(serial_port: serial.Serial, text: str) -> bool:
    data: bytes = text.encode("utf-8")
    offset: int = 0
    deadline: float = time.monotonic() + WRITE_TIMEOUT_SECONDS
    while offset < len(data):
        if time.monotonic() >= deadline:
            return False
        try:
            written: int | None = serial_port.write(data[offset:])
        except (serial.SerialException, serial.SerialTimeoutException, OSError):
            return False
        if written is None or written <= 0:
            return False
        offset += written
    return True


def close_serial(serial_port: serial.Serial, receiver: threading.Thread) -> bool:
    stop_event.set()
    success: bool = True
    try:
        if serial_port.is_open:
            serial_port.close()
    except (serial.SerialException, OSError):
        success = False
        emit_error()
    receiver.join(timeout=1.0)
    return success


def request_stop(_signal_number: int, _frame: FrameType | None) -> None:
    stop_event.set()


def run_port(path: str) -> int:
    try:
        serial_port: serial.Serial = serial.Serial(
            port=path,
            baudrate=BAUD_RATE,
            timeout=READ_TIMEOUT_SECONDS,
            write_timeout=WRITE_TIMEOUT_SECONDS,
        )
    except (serial.SerialException, OSError):
        emit_error()
        emit_close()
        return 1

    commands: queue.Queue[InputLine | None] = queue.Queue()
    reader: threading.Thread = threading.Thread(
        target=stdin_loop, args=(commands,), daemon=True
    )
    receiver: threading.Thread = threading.Thread(
        target=receive_loop, args=(serial_port,), daemon=True
    )
    reader.start()
    receiver.start()
    emit({"type": "open"})

    exit_code: int = 0
    try:
        while not stop_event.is_set():
            try:
                item: InputLine | None = commands.get(timeout=READ_TIMEOUT_SECONDS)
            except queue.Empty:
                continue
            if item is None:
                break
            line, too_long = item
            if too_long:
                emit_error()
                continue
            command: Command | None = parse_command(line)
            if command is None:
                emit_error()
                continue
            if isinstance(command, CloseCommand):
                break
            if not write_all(serial_port, command.data):
                failure_event.set()
                emit_error()
                exit_code = 1
                break
            emit({"type": "written", "id": command.identifier})
    finally:
        if failure_event.is_set():
            exit_code = 1
        if not close_serial(serial_port, receiver):
            exit_code = 1
        emit_close()
    return exit_code


def main(arguments: list[str]) -> int:
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    if arguments == ["--list"]:
        return write_list()
    if (
        len(arguments) == 2
        and arguments[0] == "--port"
        and valid_port_path(arguments[1])
    ):
        return run_port(arguments[1])
    emit_error()
    emit_close()
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
