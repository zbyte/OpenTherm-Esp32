#!/usr/bin/env python3
"""Capture device logs over the ESP32-S3 native USB-Serial/JTAG port.

The native USB port re-enumerates on every chip reset, so this keeps reopening
the port and appends everything to one file. Optionally resets the chip first
(to catch the full boot log) and sends CLI lines once the port is up.

  python tools/devlog.py -p COM5 -t 20 --reset
  python tools/devlog.py -p COM5 -t 5 --send "help"
"""
import argparse
import os
import sys
import time

import serial

ap = argparse.ArgumentParser()
ap.add_argument("-p", "--port", default="COM5")
ap.add_argument("-t", "--seconds", type=float, default=15)
ap.add_argument("--reset", action="store_true", help="reset the chip via RTS first")
ap.add_argument("--send", action="append", default=[], help="CLI line to send (repeatable)")
ap.add_argument("-o", "--out", default=None)
args = ap.parse_args()

log_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".target_logging")
os.makedirs(log_dir, exist_ok=True)
out_path = args.out or os.path.join(log_dir, time.strftime("%Y%m%d_%H%M%S") + ".log")

end = time.time() + args.seconds
pending = list(args.send)
did_reset = not args.reset
with open(out_path, "ab", buffering=0) as out:
    while time.time() < end:
        try:
            # Set DTR/RTS before opening: on the native USB-Serial/JTAG port the
            # default open toggles them, which resets the chip.
            s = serial.Serial(None, 115200, timeout=0.2, write_timeout=1)
            s.port = args.port
            s.dtr = False
            s.rts = False
            s.open()
            with s:
                if not did_reset:
                    did_reset = True
                    s.dtr = False
                    s.rts = True
                    time.sleep(0.1)
                    s.rts = False
                    continue  # port drops while the chip re-enumerates
                opened = time.time()
                while time.time() < end:
                    if pending and time.time() - opened > 1.0:
                        s.write((pending.pop(0) + "\r\n").encode())
                    data = s.read(4096)
                    if data:
                        out.write(data)
        except (serial.SerialException, OSError):
            time.sleep(0.1)

print(out_path)
