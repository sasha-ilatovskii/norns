#!/usr/bin/env python3
"""Show a short test pattern on an Adafruit 400x240 Sharp Memory Display."""

import argparse
import time

import spidev


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


def command(spi, value, vcom):
    spi.xfer2([reverse_byte(value | (VCOM if vcom else 0)), 0])


def send_frame(spi, frame, vcom):
    for first_row in range(0, HEIGHT, ROWS_PER_TRANSFER):
        packet = bytearray([reverse_byte(WRITE | (VCOM if vcom else 0))])
        for row in range(first_row, min(first_row + ROWS_PER_TRANSFER, HEIGHT)):
            packet.append(reverse_byte(row + 1))
            start = row * BYTES_PER_ROW
            packet.extend(reverse_byte(value) for value in frame[start : start + BYTES_PER_ROW])
            packet.append(0)
        packet.append(0)
        spi.xfer2(list(packet))


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
    args = parser.parse_args()

    spi = spidev.SpiDev()
    spi.open(0, 0)
    spi.mode = 0
    spi.cshigh = True
    spi.max_speed_hz = 2_000_000

    vcom = False
    try:
        send_frame(spi, make_pattern(), vcom)
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            time.sleep(min(0.5, max(0, deadline - time.monotonic())))
            vcom = not vcom
            command(spi, 0, vcom)
    finally:
        command(spi, CLEAR, vcom)
        spi.close()


if __name__ == "__main__":
    main()
