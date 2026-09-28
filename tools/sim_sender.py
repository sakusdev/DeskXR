#!/usr/bin/env python3
from __future__ import annotations

import argparse
import math
import socket
import struct
import time

MAGIC = b"DXR1"
VERSION = 1
POSE_VALID = 1

BUTTON_A = 1 << 0
BUTTON_X = 1 << 2

HEADER = struct.Struct("<4sHHI")
HAND = struct.Struct("<3f4f2f2fII")
HAPTIC = struct.Struct("<4sHHIB3x3f")
ACK = struct.Struct("<4sHHI")
PACKET_SIZE = HEADER.size + HAND.size * 2


def hand_packet(x, y, z, trigger=0.0, grip=0.0, joy_x=0.0, joy_y=0.0, buttons=0):
    return HAND.pack(
        x, y, z,
        0.0, 0.0, 0.0, 1.0,
        trigger, grip,
        joy_x, joy_y,
        buttons,
        POSE_VALID,
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=39742)
    parser.add_argument("--hz", type=float, default=72.0)
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setblocking(False)
    interval = 1.0 / max(1.0, args.hz)
    sequence = 0
    start = time.perf_counter()

    print(f"DeskXR simulator -> {args.host}:{args.port} at {args.hz:.1f} Hz")
    print("Ctrl+C to stop")

    try:
        while True:
            frame_start = time.perf_counter()
            t = frame_start - start
            wave = math.sin(t * 1.3)
            small = math.sin(t * 2.1)

            left = hand_packet(
                -0.23 + small * 0.025,
                1.32 + wave * 0.05,
                -0.48,
                trigger=(math.sin(t) + 1.0) * 0.5,
                grip=0.15,
                buttons=BUTTON_X if int(t) % 4 == 0 else 0,
            )

            right = hand_packet(
                0.23 - small * 0.025,
                1.32 - wave * 0.05,
                -0.48,
                trigger=(math.sin(t + math.pi) + 1.0) * 0.5,
                grip=0.15,
                joy_x=math.sin(t * 0.5) * 0.3,
                buttons=BUTTON_A if int(t) % 4 == 2 else 0,
            )

            packet = HEADER.pack(MAGIC, VERSION, PACKET_SIZE, sequence) + left + right
            sock.sendto(packet, (args.host, args.port))
            sequence = (sequence + 1) & 0xFFFFFFFF

            while True:
                try:
                    data, _ = sock.recvfrom(256)
                except BlockingIOError:
                    break

                if len(data) == ACK.size:
                    magic, version, size, ack_sequence = ACK.unpack(data)
                    if magic == b"DXA1" and version == VERSION and size == ACK.size:
                        print(f"PC link ack for tracking #{ack_sequence}")
                    continue

                if len(data) == HAPTIC.size:
                    magic, version, size, haptic_sequence, hand, duration, frequency, amplitude = HAPTIC.unpack(data)
                    if magic != b"DXH1" or version != VERSION or size != HAPTIC.size:
                        continue

                    hand_name = "left" if hand == 0 else "right"
                    print(
                        f"haptic #{haptic_sequence} {hand_name}: "
                        f"{duration:.3f}s {frequency:.1f}Hz amp={amplitude:.2f}"
                    )

            elapsed = time.perf_counter() - frame_start
            if elapsed < interval:
                time.sleep(interval - elapsed)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
