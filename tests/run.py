#!/usr/bin/env python3
"""Compile the real library and sketch against host-only Arduino/BLE stubs."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
compiler = shlex.split(os.environ.get("CXX", "c++"))
sketch = (ROOT / "examples/shotStopper/shotStopper.ino").read_text()

with tempfile.TemporaryDirectory(prefix="acaia-tests-") as directory:
    build = Path(directory)
    for momentary, timer_only, reed in (
        ("false", "false", "false"),
        ("true", "false", "false"),
        ("false", "true", "false"),
        ("false", "false", "true"),
    ):
        variant = sketch
        for name, value in (("MOMENTARY", momentary), ("TIMER_ONLY", timer_only),
                            ("REEDSWITCH", reed)):
            original = "#define " + name + " false"
            assert variant.count(original) == 1, "Update test configuration replacement: " + name
            variant = variant.replace(original, "#define " + name + " " + value)
        source = build / "shotStopper.ino"
        source.write_text(variant)
        executable = build / "regression"
        subprocess.run(
            compiler + [
                "-std=c++17", "-g", "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer", "-DARDUINO_ESP32S3_DEV",
                '-DSHOTSTOPPER_SKETCH="' + str(source) + '"',
                "-I" + str(ROOT / "tests/stubs"), "-I" + str(ROOT),
                str(ROOT / "tests/regression.cpp"), "-o", str(executable),
            ],
            check=True,
            timeout=120,
        )
        print("MOMENTARY={}, TIMER_ONLY={}, REEDSWITCH={}".format(
            momentary, timer_only, reed), flush=True)
        subprocess.run([str(executable)], check=True, timeout=30)
