#!/usr/bin/env python3
"""Drive a bench build's virtual paddle and tare faults, and log everything.

Requires firmware built with -DSHOTSTOPPER_BENCH=1. Commands, typed one per line
or given with --script (comma separated):

    1 / 0      virtual paddle on / off
    n          normal tares
    a          skip the shot-start tare
    b          skip the shot-start and takeover tares
    wait S     pause S seconds (useful in --script)
    quit       stop

The full log, including every weight, goes to --log. The console shows events
only, plus a weight about once a second, so it stays readable while pouring.
"""
import argparse
import os
import select
import sys
import time

from serialport import LineReader, is_weight, open_port

SEND = {"1", "0", "n", "a", "b"}


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True)
    parser.add_argument("--log", required=True, help="output file (refuses to overwrite)")
    parser.add_argument("--seconds", type=float, default=1800, help="maximum run time")
    parser.add_argument("--script", help='e.g. "a,1,wait 6,0" then continue interactively')
    args = parser.parse_args()

    fd = open_port(args.port)
    reader = LineReader(fd)
    pending = [c.strip() for c in args.script.split(",")] if args.script else []
    resume_at = 0.0
    last_shown_weight = -1.0
    interactive = sys.stdin.isatty() or not args.script

    with open(args.log, "x") as log:
        def note(text, echo=True):
            stamp = f"{reader.elapsed():8.3f}"
            log.write(f"{stamp} {text}\n")
            log.flush()
            if echo:
                print(f"{stamp} {text}", flush=True)

        note("BENCH DRIVER started; never use a bench build in a machine")
        while reader.elapsed() < args.seconds:
            if pending and reader.elapsed() >= resume_at:
                command = pending.pop(0)
                if command.startswith("wait"):
                    try:
                        resume_at = reader.elapsed() + float(command[4:])
                    except ValueError:
                        note(f"unknown command {command!r}")
                elif command == "quit":
                    break
                elif command in SEND:
                    os.write(fd, command.encode())
                    note(f">>> sent {command}")
                elif command:
                    note(f"unknown command {command!r}")

            if interactive and select.select([sys.stdin], [], [], 0)[0]:
                line = sys.stdin.readline()
                if not line:
                    interactive = False
                else:
                    pending.extend(c.strip() for c in line.split(","))

            for elapsed, line in reader.poll(0.05):
                weight = is_weight(line)
                show = not weight or elapsed - last_shown_weight >= 1.0
                if weight and show:
                    last_shown_weight = elapsed
                note(line, echo=show)
    os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
