#!/usr/bin/env python3
"""Show a short test pattern on an Adafruit 400x240 Sharp Memory Display."""

import argparse
import glob
import os
import time

WIDTH = 400
HEIGHT = 240
BYTES_PER_ROW = WIDTH // 8
ROWS_PER_TRANSFER = 60
WRITE = 0x01
VCOM = 0x02
CLEAR = 0x04


def reverse_byte(value):
    value = ((value >> 4) | (value << 4)) & 0xFF
    value = ((value & 0xCC) >> 2) | ((value & 0x33) << 2)
    return ((value & 0xAA) >> 1) | ((value & 0x55) << 1)


class ActiveHighCs:
    def __init__(self, chip_path, line_offset):
        try:
            import gpiod
        except ModuleNotFoundError as error:
            raise RuntimeError(
                "Python GPIO bindings are missing; install python3-libgpiod"
            ) from error

        self.gpiod = gpiod
        self.line_offset = line_offset
        self.request = None
        self.chip = None
        self.line = None

        if hasattr(gpiod, "request_lines"):
            from gpiod.line import Direction, Value

            chip_path = self.resolve_chip_path(chip_path)
            self.value_active = Value.ACTIVE
            self.value_inactive = Value.INACTIVE
            self.request = gpiod.request_lines(
                chip_path,
                consumer="norns-sharp-memory-cs",
                config={
                    line_offset: gpiod.LineSettings(
                        direction=Direction.OUTPUT,
                        output_value=Value.INACTIVE,
                    )
                },
            )
        else:
            self.value_active = 1
            self.value_inactive = 0
            self.chip = gpiod.Chip(chip_path)
            self.line = self.chip.get_line(line_offset)
            self.line.request(
                consumer="norns-sharp-memory-cs",
                type=gpiod.LINE_REQ_DIR_OUT,
                default_vals=[0],
            )

    def resolve_chip_path(self, selector):
        if selector.startswith("/"):
            return selector

        device_path = f"/dev/{selector}"
        if os.path.exists(device_path):
            return device_path

        for path in sorted(glob.glob("/dev/gpiochip*")):
            try:
                with self.gpiod.Chip(path) as chip:
                    info = chip.get_info()
                    if selector in (info.name, info.label):
                        return path
            except OSError:
                continue
        raise RuntimeError(f"GPIO chip '{selector}' was not found")

    def set_value(self, value):
        if self.request is not None:
            self.request.set_value(self.line_offset, value)
        else:
            self.line.set_value(value)

    def transfer(self, spi, data):
        self.set_value(self.value_active)
        try:
            return spi.xfer2(list(data))
        finally:
            self.set_value(self.value_inactive)

    def transfer_fragments(self, spi, fragments):
        self.set_value(self.value_active)
        try:
            for fragment in fragments:
                spi.xfer2(list(fragment))
        finally:
            self.set_value(self.value_inactive)

    def close(self):
        self.set_value(self.value_inactive)
        if self.request is not None:
            self.request.release()
        else:
            self.line.release()
            self.chip.close()


def command(spi, cs, value, vcom):
    cs.transfer(spi, [reverse_byte(value | (VCOM if vcom else 0)), 0])


def send_frame(spi, cs, frame, vcom):
    fragments = []
    for first_row in range(0, HEIGHT, ROWS_PER_TRANSFER):
        packet = bytearray()
        if first_row == 0:
            packet.append(reverse_byte(WRITE | (VCOM if vcom else 0)))
        for row in range(first_row, min(first_row + ROWS_PER_TRANSFER, HEIGHT)):
            packet.append(reverse_byte(row + 1))
            start = row * BYTES_PER_ROW
            packet.extend(reverse_byte(value) for value in frame[start : start + BYTES_PER_ROW])
            packet.append(0)
        if first_row + ROWS_PER_TRANSFER >= HEIGHT:
            packet.append(0)
        fragments.append(packet)
    cs.transfer_fragments(spi, fragments)


def make_pattern():
    frame = bytearray(WIDTH * HEIGHT // 8)
    for y in range(HEIGHT):
        for x in range(WIDTH):
            white = ((x // 25) + (y // 20)) % 2 == 0
            if white:
                frame[y * BYTES_PER_ROW + x // 8] |= 1 << (x & 7)
    return frame


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--cs-chip", default="pinctrl-rp1")
    parser.add_argument("--cs-line", type=int, default=25)
    args = parser.parse_args()

    try:
        import spidev
    except ModuleNotFoundError as error:
        parser.error("Python SPI bindings are missing; install python3-spidev")

    spi = spidev.SpiDev()
    spi.open(0, 0)
    spi.mode = 0
    spi.max_speed_hz = 2_000_000
    try:
        cs = ActiveHighCs(args.cs_chip, args.cs_line)
    except Exception:
        spi.close()
        raise

    vcom = False
    try:
        send_frame(spi, cs, make_pattern(), vcom)
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            time.sleep(min(0.5, max(0, deadline - time.monotonic())))
            vcom = not vcom
            command(spi, cs, 0, vcom)
    finally:
        try:
            command(spi, cs, CLEAR, vcom)
        finally:
            try:
                cs.close()
            finally:
                spi.close()


if __name__ == "__main__":
    main()
