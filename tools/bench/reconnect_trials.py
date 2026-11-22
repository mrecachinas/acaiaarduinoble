#!/usr/bin/env python3
"""Reset the board repeatedly and measure how long it takes to stream weights.

Timing starts at the firmware's "device active" banner after each reset, so
output buffered before the reset is never counted. A trial passes once
--weights readings arrive within --limit seconds of that banner.
"""
import argparse
import os
import statistics
import sys
import time

from serialport import LineReader, is_weight, open_port, reset_board

EVENTS = {
    "connects": lambda line: line == "Connected",
    "discovery_failures": lambda line: "Attribute discovery failed" in line,
    "silent_connections": lambda line: "no weight since connect" in line,
    "timeouts": lambda line: "timeout!" in line,
    "crashes": lambda line: "Guru Meditation" in line,
}


def trial(port, limit, needed, log, number):
    fd = open_port(port)
    reset_board(fd)
    reader = LineReader(fd)
    booted = False
    counts = dict.fromkeys(EVENTS, 0)
    first_weight = None
    weights = 0
    deadline = time.monotonic() + limit + 10  # allow time for the reset itself
    while time.monotonic() < deadline and weights < needed:
        for _, line in reader.poll():
            if not booted:
                log.write(f"T{number} pre-boot {line}\n")
                if "device active" in line:
                    booted = True
                    reader.restart_clock()
                    deadline = time.monotonic() + limit
                continue
            log.write(f"T{number} {reader.elapsed():7.3f} {line}\n")
            if is_weight(line):
                weights += 1
                if first_weight is None:
                    first_weight = reader.elapsed()
            for key, matches in EVENTS.items():
                counts[key] += matches(line)
    os.close(fd)
    return booted and weights >= needed, booted, first_weight, counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--trials", type=int, default=10)
    parser.add_argument("--limit", type=float, default=60, help="seconds per trial")
    parser.add_argument("--weights", type=int, default=30, help="readings required")
    parser.add_argument("--log", required=True, help="output file (refuses to overwrite)")
    args = parser.parse_args()

    times = []
    passed = 0
    with open(args.log, "x") as log:
        for number in range(1, args.trials + 1):
            ok, booted, first, counts = trial(args.port, args.limit, args.weights, log, number)
            passed += ok
            if ok:
                times.append(first)
            status = "OK" if ok else ("NO RESET" if not booted else "FAIL")
            first_text = f"{first:.1f}s" if first is not None else "-"
            detail = " ".join(f"{k}={v}" for k, v in counts.items())
            print(f"trial {number}: {status} first_weight={first_text} {detail}", flush=True)
            time.sleep(3)
    print(f"\n{passed}/{args.trials} reached {args.weights} weights within {args.limit:.0f}s")
    if times:
        print(f"time to first weight: min {min(times):.1f}s, "
              f"median {statistics.median(times):.1f}s, max {max(times):.1f}s")
    return 0 if passed == args.trials else 2


if __name__ == "__main__":
    sys.exit(main())
