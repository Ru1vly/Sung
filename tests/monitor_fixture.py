#!/usr/bin/env python3
"""Stands in for parec where no speaker plays: whatever arguments it gets, it
writes 48 kHz stereo float32 in real time, a bass note swelling twice a second
under a high one, the way music would reach the output tap."""
import math
import struct
import sys
import time

frame, start = 0, time.monotonic()
out = sys.stdout.buffer
try:
    while True:
        chunk = bytearray()
        for i in range(1024):
            t = (frame + i) / 48000
            v = 0.3 * math.sin(2 * math.pi * 110 * t) * (0.6 + 0.4 * math.sin(2 * math.pi * 2 * t)) \
                + 0.15 * math.sin(2 * math.pi * 2500 * t)
            chunk += struct.pack("<ff", v, v)
        out.write(chunk)
        out.flush()
        frame += 1024
        # Real time, as a recording arrives.
        time.sleep(max(0.0, start + frame / 48000 - time.monotonic()))
except (BrokenPipeError, KeyboardInterrupt):
    pass
