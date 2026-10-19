import os
from pathlib import Path
import shutil
import subprocess
import unittest

from test_diagnostics import d


class NativeTests(unittest.TestCase):
    def test_actual_sketch_gatt_rate_and_debugger_disconnect(self):
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable: sketch host test not run")
        root = Path(__file__).resolve().parents[1]
        binary = root / "tests" / (".diagnostics-sketch-test-" + str(os.getpid()))
        try:
            subprocess.run([compiler, "-std=c++17", "-I", str(root / "tests" / "host_stubs"),
                            "-I", str(root), str(root / "tests" / "diagnostics_sketch_host.cpp"),
                            "-o", str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True, capture_output=True)
        finally:
            binary.unlink(missing_ok=True)

    def test_header_and_actual_wire_interoperability(self):
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable: native header test not run")
        root = Path(__file__).resolve().parents[1]
        binary = root / "tests" / (".diagnostics-host-test-" + str(os.getpid()))
        try:
            subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            str(root / "tests" / "diagnostics_host.cpp"), "-o", str(binary)],
                           check=True, capture_output=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True)
        finally:
            binary.unlink(missing_ok=True)
        wire = result.stdout
        self.assertEqual(len(wire), 20 + 2 * (20 + 496))
        status = d.decode_status(wire[:20])
        self.assertEqual(status.flags, 0x7F)
        self.assertEqual(status.uptime_ms, 0x12345678)
        self.assertEqual(status.weight_decigrams, -32768)
        self.assertEqual(status.target_decigrams, 65535)
        self.assertEqual(status.actual_offset_decigrams, -123)
        self.assertEqual(status.weight_age_ms, d.MASK - 1)
        self.assertEqual(status.shot_elapsed_deciseconds, 65535)
        empty = d.decode_metadata(wire[20:40])
        self.assertEqual(d.decode_history(wire[40:536], empty), [])
        full = d.decode_metadata(wire[536:556])
        self.assertEqual((full.count, full.oldest, full.next, full.overwritten, full.generation),
                         (24, 3, 27, 3, 3))
        events = d.decode_history(wire[556:], full)
        self.assertEqual([event.seq for event in events], list(range(3, 27)))
        self.assertEqual(events[-1].value_decigrams, -1026)
        self.assertEqual(events[-1].sample_age_ms, d.UNKNOWN_AGE)


if __name__ == "__main__":
    unittest.main()
