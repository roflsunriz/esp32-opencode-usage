"""Read the LCD's real framebuffer over USB without resetting the device."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import time

from PIL import Image
import serial


def capture(port_name: str, output: Path) -> None:
    connection = serial.Serial(
        port=None, baudrate=115200, timeout=0.25, write_timeout=3
    )
    connection.dtr = False
    connection.rts = False
    connection.port = port_name
    connection.open()
    try:
        connection.write(b'{"version":1,"type":"screenshot"}\n')
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            try:
                header: object = json.loads(connection.readline())
            except (ValueError, UnicodeDecodeError):
                continue
            if isinstance(header, dict) and header.get("type") == "screenshot":
                if (header.get("width"), header.get("height"), header.get("bytes")) != (
                    320,
                    240,
                    230400,
                ):
                    raise ValueError("Unexpected framebuffer dimensions")
                break
        else:
            raise TimeoutError("LCD capture header was not received")
        pixels = bytearray()
        deadline = time.monotonic() + 60
        while len(pixels) < 230400 and time.monotonic() < deadline:
            pixels.extend(connection.read(min(4096, 230400 - len(pixels))))
        if len(pixels) != 230400:
            raise TimeoutError("LCD capture was incomplete")
        output.parent.mkdir(parents=True, exist_ok=True)
        Image.frombytes("RGB", (320, 240), bytes(pixels)).save(output)
    finally:
        connection.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", type=Path, default=Path(".private/lcd.png"))
    arguments = parser.parse_args()
    capture(arguments.port, arguments.output)
    print(f"LCD image saved: {arguments.output}")
