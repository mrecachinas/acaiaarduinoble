import asyncio
import io
import json
import subprocess
import tempfile
import threading
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import runner

BASELINE = "9c6c34420bf184eec7bb9a542c17d960d06d4443"
RELEASE = "6c497bbcf752b3b0cb70bf6036013d4345a65c25"
HAS_BASELINE = subprocess.run(
    ["git", "cat-file", "-e", BASELINE + "^{commit}"], cwd=runner.ROOT,
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
).returncode == 0
HAS_RELEASE = subprocess.run(
    ["git", "cat-file", "-e", RELEASE + "^{commit}"], cwd=runner.ROOT,
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
).returncode == 0


class RecordingBackend:
    def __init__(self):
        self.connected = True
        self.calls = []
        self.block_start = False
        self.start_seen = asyncio.Event()

    async def handle(self, op, arg):
        self.calls.append((op, arg))
        if op == "CHAR":
            return "12"
        if self.block_start and op == "WRITE" and arg.endswith(runner.START.hex()):
            self.start_seen.set()
            await asyncio.Event().wait()
        return "1"

    async def close(self):
        self.connected = False


class QueuedStopBackend(RecordingBackend):
    def __init__(self, properties=12):
        super().__init__()
        self.properties = properties
        self.stop_delivered = False
        self.pending_stop = None

    async def handle(self, op, arg):
        if op == "CHAR":
            self.calls.append((op, arg))
            return str(self.properties)
        if op == "WRITE" and arg.endswith(runner.STOP.hex()):
            self.calls.append((op, arg))
            async def deliver():
                await asyncio.sleep(0.03)
                if self.connected:
                    self.stop_delivered = True
            if arg.split("\t")[1] == "1":
                await deliver()
            else:
                self.pending_stop = asyncio.create_task(deliver())
            return "1"
        return await super().handle(op, arg)

    async def close(self):
        self.connected = False
        if self.pending_stop:
            self.pending_stop.cancel()
            try:
                await self.pending_stop
            except asyncio.CancelledError:
                pass


class CleanupDeliveryTests(unittest.IsolatedAsyncioTestCase):
    async def test_cleanup_delivers_stop_before_disconnect(self):
        for properties in (12, 4):
            with self.subTest(properties=properties):
                backend = QueuedStopBackend(properties)
                session = runner.Session(backend, runner.Trace(io.StringIO(), console=False), True)
                await session.rpc("WRITE", runner.TX + "\t0\t" + runner.START.hex())
                await session.close()
                self.assertTrue(backend.stop_delivered)
                self.assertFalse(backend.connected)
                expected_response = "1" if properties & 8 else "0"
                self.assertIn(("WRITE", runner.TX + "\t" + expected_response + "\t" + runner.STOP.hex()),
                              backend.calls)

    async def test_firmware_stop_is_also_drained(self):
        backend = QueuedStopBackend()
        session = runner.Session(backend, runner.Trace(io.StringIO(), console=False), True)
        await session.rpc("WRITE", runner.TX + "\t0\t" + runner.START.hex())
        await session.rpc("WRITE", runner.TX + "\t0\t" + runner.STOP.hex())
        await session.close()
        self.assertTrue(backend.stop_delivered)

    async def test_lost_link_does_not_claim_cleanup_success(self):
        backend = RecordingBackend()
        session = runner.Session(backend, runner.Trace(io.StringIO(), console=False), True)
        await session.rpc("WRITE", runner.TX + "\t0\t" + runner.START.hex())
        backend.connected = False
        with self.assertRaisesRegex(RuntimeError, "timer stop"):
            await session.close()
        self.assertFalse(backend.connected)

    async def test_cleanup_notifications_are_not_labelled_delivered(self):
        output = io.StringIO()
        session = runner.Session(RecordingBackend(), runner.Trace(output, console=False))
        closing = threading.Event()
        closing.set()
        deliveries = []
        session.native = SimpleNamespace(closing=closing, notify=lambda *args: deliveries.append(args))
        session.receive(runner.RX, b"\x01")
        event = json.loads(output.getvalue().splitlines()[0])
        self.assertFalse(event["delivered"])
        self.assertEqual(deliveries, [])


class PolicyTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.backend = RecordingBackend()
        self.trace = runner.Trace(io.StringIO(), console=False)
        self.session = runner.Session(self.backend, self.trace)

    async def asyncTearDown(self):
        await self.session.close()

    async def test_monitor_rejects_commands_and_controls(self):
        for packet in runner.CONTROL_PACKETS:
            with self.assertRaises(PermissionError):
                await self.session.rpc("WRITE", runner.TX + "\t0\t" + packet.hex())
        for command in ("paddle on", "paddle off", "drop on", "goal 40"):
            with self.assertRaises(PermissionError):
                self.session.control(command)
        self.assertEqual(self.backend.calls, [])

    async def test_only_known_packets_and_command_characteristics(self):
        self.session.allow_commands = True
        for uuid, packet in ((runner.RX, runner.TARE), (runner.TX, b"\0")):
            with self.assertRaises(PermissionError):
                await self.session.rpc("WRITE", uuid + "\t0\t" + packet.hex())
        self.assertEqual(self.backend.calls, [])

    async def test_monitor_allows_handshake(self):
        for packet in runner.MONITOR_PACKETS:
            await self.session.rpc("WRITE", runner.TX + "\t1\t" + packet.hex())
        self.assertEqual(len(self.backend.calls), 3)

    async def test_cancelled_start_still_gets_cleanup_stop(self):
        self.session.allow_commands = True
        self.backend.block_start = True
        task = asyncio.create_task(self.session.rpc("WRITE", runner.TX + "\t0\t" + runner.START.hex()))
        await self.backend.start_seen.wait()
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        await self.session.close()
        self.assertTrue(any(arg.endswith(runner.STOP.hex()) for op, arg in self.backend.calls
                            if op == "WRITE"))
        self.assertFalse(self.backend.connected)


class NativeIntegrationTests(unittest.IsolatedAsyncioTestCase):
    async def wait_for(self, condition, seconds=8):
        deadline = asyncio.get_running_loop().time() + seconds
        while not condition():
            if asyncio.get_running_loop().time() >= deadline:
                self.fail("Timed out waiting for native firmware condition")
            await asyncio.sleep(0.02)

    async def test_main_and_candidate_on_identical_synthetic_fragments(self):
        # The real source files are selected independently; no firmware logic is rewritten.
        cases = [(None, True)]
        if HAS_BASELINE:
            cases.insert(0, (BASELINE, False))
        for ref, expected_takeover in cases:
            with self.subTest(ref=ref), tempfile.TemporaryDirectory(prefix="lunar-integration-") as directory:
                library, manifest = runner.build_native(directory, ref)
                self.assertEqual(set(manifest["sha256"]), set(runner.SOURCES))
                output = io.StringIO()
                trace = runner.Trace(output, console=False)
                backend = runner.FakeBLE()
                session = runner.Session(backend, trace, allow_commands=True, fragment_size=3)
                await session.start(library)
                try:
                    result = await asyncio.wait_for(session.cycle(), 15)
                    self.assertEqual(result, expected_takeover)
                    self.assertGreater(session.raw_count, 10)
                    self.assertEqual(session.saw_weight, expected_takeover)
                    self.assertEqual(trace.shot_starts, 1)
                finally:
                    await session.close()
                self.assertFalse(backend.connected)
                self.assertFalse(session.native.thread.is_alive())
                events = [json.loads(line) for line in output.getvalue().splitlines()]
                writes = [event["hex"] for event in events if event["event"] == "write_requested"]
                self.assertIn(runner.IDENTIFY.hex(), writes)
                self.assertIn(runner.NOTIFICATIONS.hex(), writes)
                self.assertIn(runner.TARE.hex(), writes)

    @unittest.skipUnless(HAS_RELEASE, "Release comparison requires the v3.3.0 commit in local history")
    async def test_v330_preserves_release_writes_and_source(self):
        with tempfile.TemporaryDirectory(prefix="lunar-release-") as directory:
            library, manifest = await asyncio.to_thread(runner.build_native, directory, RELEASE)
            self.assertEqual(manifest["base_commit"], RELEASE)
            for name, digest in manifest["sha256"].items():
                original = subprocess.check_output(["git", "show", RELEASE + ":" + name], cwd=runner.ROOT)
                self.assertEqual(runner.hashlib.sha256(original).hexdigest(), digest)
            output = io.StringIO()
            backend = runner.FakeBLE()
            session = runner.Session(backend, runner.Trace(output, console=False), allow_commands=True)
            await session.start(library)
            try:
                self.assertTrue(await asyncio.wait_for(session.cycle(), 15))
                self.assertTrue(session.saw_weight)
            finally:
                await session.close()
            events = [json.loads(line) for line in output.getvalue().splitlines()]
            writes = [event for event in events if event["event"] == "write_requested"]
            self.assertTrue(writes)
            self.assertTrue(all(event["response"] for event in writes))
            self.assertEqual(sum(event["hex"] == runner.TARE.hex() for event in writes), 2)
            self.assertFalse(session.native.thread.is_alive())
            self.assertFalse(backend.connected)

    @unittest.skipUnless(HAS_BASELINE, "Historical-main comparison requires its commit in local history")
    def test_historical_ref_provenance(self):
        with tempfile.TemporaryDirectory(prefix="lunar-provenance-") as directory:
            _, manifest = runner.build_native(directory, BASELINE)
            self.assertEqual(manifest["base_commit"], BASELINE)
            for name, digest in manifest["sha256"].items():
                original = subprocess.check_output(["git", "show", BASELINE + ":" + name], cwd=runner.ROOT)
                self.assertEqual(runner.hashlib.sha256(original).hexdigest(), digest)

    async def test_watchdog_reconnect_is_driven_by_real_firmware(self):
        with tempfile.TemporaryDirectory(prefix="lunar-reconnect-") as directory:
            library, _ = runner.build_native(directory)
            output = io.StringIO()
            trace = runner.Trace(output, console=False)
            backend = runner.FakeBLE()
            session = runner.Session(backend, trace, allow_commands=True)
            await session.start(library)
            try:
                await self.wait_for(lambda: session.saw_weight)
                session.control("drop on")
                await self.wait_for(lambda: '"message": "timeout!"' in output.getvalue(), seconds=8)
                await self.wait_for(lambda: output.getvalue().count('"operation": "CONNECT"') >= 4)
                self.assertEqual(trace.shot_starts, 0)
                session.control("drop off")
                await self.wait_for(lambda: session.native.snapshot().get("last_weight_ms", 0) > 6000)
            finally:
                await session.close()
            self.assertFalse(session.native.thread.is_alive())

    async def test_warm_cycle_waits_for_actual_decoded_weights(self):
        class DelayedWeights(runner.FakeBLE):
            def __init__(self):
                super().__init__()
                self.release_weights = asyncio.Event()

            async def _weights(self):
                await self.release_weights.wait()
                await super()._weights()

        with tempfile.TemporaryDirectory(prefix="lunar-warm-start-") as directory:
            library, _ = await asyncio.to_thread(runner.build_native, directory)
            output = io.StringIO()
            trace = runner.Trace(output, console=False)
            backend = DelayedWeights()
            session = runner.Session(backend, trace, allow_commands=True)
            await session.start(library)
            cycle = asyncio.create_task(session.cycle(wait_for_weights=True))
            try:
                await self.wait_for(lambda: session.native.snapshot().get("connected"))
                await asyncio.sleep(0.4)
                self.assertEqual(trace.shot_starts, 0)
                self.assertNotIn('"event": "control"', output.getvalue())
                backend.release_weights.set()
                self.assertTrue(await asyncio.wait_for(cycle, 10))
                events = [json.loads(line) for line in output.getvalue().splitlines()]
                ready = next(e for e in events if e["event"] == "warm_start_ready")
                paddle = next(e for e in events if e["event"] == "control")
                self.assertGreaterEqual(ready["decoded_updates"], 3)
                self.assertLessEqual(ready["elapsed_s"], paddle["elapsed_s"])
            finally:
                cycle.cancel()
                await asyncio.gather(cycle, return_exceptions=True)
                await session.close()


class SupervisorTests(unittest.IsolatedAsyncioTestCase):
    async def test_cycle_abort_cancels_actions_and_closes(self):
        class StubSession:
            def __init__(self, *args):
                self.finished = asyncio.Event()
                self.closed = self.cancelled = False
                self.saw_weight = False
                self.raw_count = 0

            async def start(self, library):
                asyncio.get_running_loop().call_later(0.02, self.finished.set)

            async def cycle(self, wait_for_weights=False):
                try:
                    await asyncio.Event().wait()
                except asyncio.CancelledError:
                    self.cancelled = True
                    raise

            async def close(self):
                self.closed = True

        session = StubSession()
        args = SimpleNamespace(fake=True, device=None, allow_scale_commands=True,
                               fragment_size=0, interactive=False, scenario="cycle", seconds=1)
        with patch.object(runner, "Session", return_value=session):
            result = await runner.run(args, None, {}, runner.Trace(io.StringIO(), console=False))
        self.assertFalse(result)
        self.assertTrue(session.closed and session.cancelled)

    async def test_start_failure_still_closes(self):
        class StubSession:
            def __init__(self):
                self.closed = False
                self.raw_count = 0
                self.saw_weight = False
                self.finished = asyncio.Event()

            async def start(self, library):
                raise RuntimeError("startup failure")

            async def close(self):
                self.closed = True

        session = StubSession()
        args = SimpleNamespace(fake=True, device=None, allow_scale_commands=False,
                               fragment_size=0, interactive=False, scenario="monitor", seconds=1)
        with patch.object(runner, "Session", return_value=session):
            with self.assertRaisesRegex(RuntimeError, "startup failure"):
                await runner.run(args, None, {}, runner.Trace(io.StringIO(), console=False))
        self.assertTrue(session.closed)


if __name__ == "__main__":
    unittest.main()
