"""Live WiFi CSI presence detector (Mac side).

The board now detects presence by itself (LED + dashboard at http://presence.local/).
This script is for experimenting and for recording raw data: it reads the CSI
stream over USB and runs the same detection on the Mac.

    python presence.py                  # uses saved calibration, or calibrates first
    python presence.py --calibrate      # redo the empty-room calibration
    python presence.py --log data.csv   # also record raw samples (for training later)
"""

import argparse
import collections
import csv
import json
import sys
import time
from pathlib import Path

import numpy as np
import serial
from serial.tools import list_ports

BAUD = 460800
CALIBRATION_FILE = Path(__file__).with_name("calibration.json")

WINDOW_S = 2.0        # motion is measured over this sliding window
HOLD_S = 10.0         # stay "present" this long after the last movement
CALIBRATE_S = 30.0    # length of the empty-room calibration
THRESHOLD_MARGIN = 1.5
MIN_SAMPLES = 6       # need at least this many samples in a window to score it


def find_port():
    """Pick the first USB serial port that looks like an ESP32 board."""
    for p in list_ports.comports():
        if any(k in p.device for k in ("usbserial", "usbmodem", "ttyUSB", "ttyACM")):
            return p.device
    sys.exit("No ESP32 serial port found. Plug the board in or pass --port.")


def read_samples(port):
    """Yield (timestamp, rssi, amplitudes) for every CSI line; print status lines."""
    ser = serial.Serial(port, BAUD, timeout=1)
    ser.write(b"S")  # ask the board to stream raw samples
    while True:
        line = ser.readline().decode(errors="ignore").strip()
        if line.startswith("#"):
            if not line.startswith(("# stats", "# presence")):
                print(f"\r\033[K[board] {line[2:]}")
            continue
        if not line.startswith("CSI,"):
            continue
        parts = line.split(",")
        try:
            t = int(parts[1]) / 1000  # board clock, immune to serial buffering delays
            rssi = int(parts[2])
            amps = np.array([int(x) for x in parts[3:]], dtype=float)
        except ValueError:
            continue
        yield t, rssi, amps


class MotionScore:
    """Average per-subcarrier fluctuation over a sliding time window."""

    def __init__(self):
        self.window = collections.deque()
        self.active = None  # mask of subcarriers that carry signal

    def add(self, t, amps):
        if self.active is None:
            self.active = amps > 0
        mean = amps[self.active].mean()
        if mean <= 0:
            return
        # Normalising by the overall level removes the radio's automatic gain changes.
        self.window.append((t, amps[self.active] / mean))
        # Keep a minimum number of samples so slow (passive) streams still get a score.
        while len(self.window) > MIN_SAMPLES and t - self.window[0][0] > WINDOW_S:
            self.window.popleft()

    def value(self):
        if len(self.window) < MIN_SAMPLES:
            return None
        frames = np.array([a for _, a in self.window])
        return float(frames.std(axis=0).mean() * 100)

    def rate(self):
        if len(self.window) < 2:
            return 0.0
        span = self.window[-1][0] - self.window[0][0]
        return (len(self.window) - 1) / span if span > 0 else 0.0


def calibrate(samples, score):
    print(f"Calibrating: leave the room empty and still for {CALIBRATE_S:.0f} seconds...")
    for remaining in range(5, 0, -1):
        print(f"\r\033[K  starting in {remaining}s (leave now!)", end="", flush=True)
        time.sleep(1)
    print()
    values = []
    start = None
    for t, _, amps in samples:
        start = t if start is None else start
        score.add(t, amps)
        v = score.value()
        if v is not None:
            values.append(v)
        left = CALIBRATE_S - (t - start)
        print(f"\r\033[K  recording... {max(left, 0):4.0f}s left  ({score.rate():.0f} samples/s)",
              end="", flush=True)
        if left <= 0:
            break
    print()
    if len(values) < 10:
        sys.exit("Not enough CSI samples to calibrate. Is the board streaming? "
                 "(Passive mode may be too slow; see README.)")
    baseline = float(np.percentile(values, 95))
    threshold = baseline * THRESHOLD_MARGIN
    CALIBRATION_FILE.write_text(json.dumps({"baseline": baseline, "threshold": threshold}))
    print(f"Done. Empty-room level {baseline:.2f}, threshold {threshold:.2f} "
          f"(saved to {CALIBRATION_FILE.name})\n")
    return threshold


def bar(value, threshold, width=40):
    full_scale = threshold * 3
    filled = min(int(value / full_scale * width), width)
    mark = min(int(threshold / full_scale * width), width - 1)
    cells = ["█" if i < filled else "·" for i in range(width)]
    cells[mark] = "|"
    return "".join(cells)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="serial port (default: auto-detect)")
    parser.add_argument("--calibrate", action="store_true", help="redo empty-room calibration")
    parser.add_argument("--log", metavar="CSV", help="record raw samples to this CSV file")
    args = parser.parse_args()

    samples = read_samples(args.port or find_port())
    score = MotionScore()

    if args.calibrate or not CALIBRATION_FILE.exists():
        threshold = calibrate(samples, score)
    else:
        threshold = json.loads(CALIBRATION_FILE.read_text())["threshold"]
        print(f"Using saved threshold {threshold:.2f} (run with --calibrate to redo)\n")

    log_writer = None
    if args.log:
        log_file = open(args.log, "a", newline="", buffering=1)
        log_writer = csv.writer(log_file)

    last_motion = 0.0
    last_draw = 0.0
    try:
        for t, rssi, amps in samples:
            score.add(t, amps)
            if log_writer:
                log_writer.writerow([f"{t:.3f}", rssi, *amps.astype(int)])
            v = score.value()
            if v is None or t - last_draw < 0.2:
                continue
            last_draw = t
            if v > threshold:
                last_motion = t
            present = t - last_motion < HOLD_S
            state = "\033[1;32m  PRESENT (1+)\033[0m" if present else "\033[1;90m  EMPTY (0)   \033[0m"
            print(f"\r\033[K{state}  motion {v:6.2f} {bar(v, threshold)}  "
                  f"{score.rate():3.0f} samples/s  rssi {rssi}", end="", flush=True)
    except KeyboardInterrupt:
        print()


if __name__ == "__main__":
    main()
