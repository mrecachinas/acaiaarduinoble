#!/usr/bin/env python3
"""Log the ShotStopper's serial output with timestamps, optionally after a reset."""
import argparse
import sys

from serialport import LineReader, open_port, reset_board


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="e.g. /dev/cu.usbmodem2101")
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--log", required=True, help="output file (refuses to overwrite)")
    parser.add_argument("--reset", action="store_true", help="reset the board first")
    parser.add_argument("--quiet", action="store_true", help="do not echo lines")
    args = parser.parse_args()

    fd = open_port(args.port)
    if args.reset:
        reset_board(fd)
    reader = LineReader(fd)
    with open(args.log, "x") as log:
        while reader.elapsed() < args.seconds:
            for elapsed, line in reader.poll():
                log.write(f"{elapsed:8.3f} {line}\n")
                log.flush()
                if not args.quiet:
                    print(f"{elapsed:8.3f} {line}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
