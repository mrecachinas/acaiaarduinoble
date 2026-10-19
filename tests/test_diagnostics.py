import contextlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import unittest
from unittest.mock import patch
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import shotstopper_diagnostics as d


def status(flags=3, age=0, uptime=100):
    return struct.pack("<BBHIhHhIH", 1, 1, flags, uptime, -23, 360, -2, age, 65535)


def snapshot(count=2, next_seq=2, generation=4, overwritten=0):
    oldest = (next_seq - count) & d.MASK
    meta = struct.pack("<BBBBIIII", 1, 2, 24, count, generation, oldest, next_seq, overwritten)
    history = struct.pack("<BBHIII", 1, 3, count, generation, oldest, next_seq)
    for index in range(count):
        history += struct.pack("<IIBBhiI", (oldest + index) & d.MASK,
                               200 + index, 6, 0, -123, -2345, d.UNKNOWN_AGE)
    return meta, history + bytes((24 - count) * 20)


class Characteristic:
    def __init__(self, properties):
        self.properties = properties


class Service:
    def __init__(self):
        self.chars = {d.STATUS: Characteristic(["read"]),
                      d.METADATA: Characteristic(["read"]),
                      d.HISTORY: Characteristic(["read"])}

    def get_characteristic(self, uuid):
        return self.chars.get(uuid)


class Services:
    def __init__(self):
        self.service = Service()

    def get_service(self, uuid):
        return self.service if uuid == d.SERVICE else None


class FakeClient:
    instances = []

    def __init__(self, device=None, reads=None):
        self.services = Services()
        self.reads = reads
        self.calls = []
        self.is_connected = True
        self.instances.append(self)

    async def __aenter__(self):
        return self

    async def __aexit__(self, *args):
        self.is_connected = False

    async def read_gatt_char(self, uuid):
        self.calls.append(("read", uuid))
        if self.reads is not None:
            expected, data = self.reads.pop(0)
            assert expected == uuid
            return data
        meta, history = snapshot()
        return {d.STATUS: status(), d.METADATA: meta, d.HISTORY: history}[uuid]

    async def start_notify(self, uuid, callback):
        raise AssertionError("client must never subscribe or write a CCCD")

    async def stop_notify(self, uuid):
        raise AssertionError("client must never manage notifications")

    async def write_gatt_char(self, *args, **kwargs):
        raise AssertionError("client must never write a characteristic")

    async def write_gatt_descriptor(self, *args, **kwargs):
        raise AssertionError("client must never write a descriptor/CCCD")


class DisconnectingClient(FakeClient):
    async def read_gatt_char(self, uuid):
        result = await super().read_gatt_char(uuid)
        if uuid == d.HISTORY:
            self.is_connected = False
        return result


class Device:
    name = "shotStopper"
    address = "test-device"


class Advertisement:
    local_name = "shotStopper"


class Scanner:
    @staticmethod
    async def discover(**kwargs):
        return {"test": (Device(), Advertisement())}


class ProtocolTests(unittest.TestCase):
    def test_exact_status_units_and_flags(self):
        s = d.decode_status(status())
        self.assertEqual(s.weight_decigrams, -23)
        self.assertEqual(s.actual_offset_decigrams, -2)
        self.assertEqual(s.shot_elapsed_deciseconds, 65535)
        self.assertFalse(s.report()["stale"])
        for data in (status()[:-1], status() + b"\0", bytes([2]) + status()[1:],
                     bytes([1, 8]) + status()[2:], status(flags=128),
                     status(flags=3, age=d.UNKNOWN_AGE), status(flags=1, age=0)):
            with self.subTest(data=data), self.assertRaises(d.ProtocolError):
                d.decode_status(data)

    def test_stale_and_notready(self):
        for flags, age, stale, notready in ((3, 5000, False, False),
                (3, 5001, True, False), (2, 0, True, True),
                (1, d.UNKNOWN_AGE, True, True),
                (3, d.MASK - 1, True, False)):
            report = d.decode_status(status(flags, age)).report()
            self.assertEqual((report["stale"], report["not_ready"]), (stale, notready))

    def test_rollover_history(self):
        meta, data = snapshot(3, 1)
        events = d.decode_history(data, d.decode_metadata(meta))
        self.assertEqual([e.seq for e in events], [d.MASK - 1, d.MASK, 0])
        self.assertEqual(events[0].value_decigrams, -2345)
        self.assertEqual(events[0].actual_offset_decigrams, -123)
        for count in (0, 24):
            meta, data = snapshot(count, count)
            self.assertEqual(len(d.decode_history(data, d.decode_metadata(meta))), count)

    def test_invalid_metadata(self):
        good, _ = snapshot()
        for index, value in ((0, 2), (1, 3), (2, 23), (3, 25), (8, 17)):
            bad = bytearray(good)
            bad[index] = value
            with self.subTest(index=index), self.assertRaises(d.ProtocolError):
                d.decode_metadata(bad)

    def test_invalid_history(self):
        rawmeta, good = snapshot()
        meta = d.decode_metadata(rawmeta)
        for index, value in ((0, 2), (1, 4), (2, 25), (4, 9),
                             (8, 9), (12, 9), (16, 8), (24, 14),
                             (25, 1), (56, 1)):
            bad = bytearray(good)
            bad[index] = value
            with self.subTest(index=index), self.assertRaises(d.ProtocolError):
                d.decode_history(bad, meta)
        with self.assertRaises(d.ProtocolError):
            d.decode_history(good[:-1], meta)

    def test_event_details_exhaustive(self):
        valid = {1: {0}, 2: set(range(4)), 3: set(range(1, 9)), 4: {0, 1},
                 5: {0}, 6: {0}, 7: set(range(5)), 8: {0, 1},
                 9: set(range(1, 8)) | set(range(129, 135)), 10: {0, 1},
                 11: {0}, 12: {0}, 13: {0}}
        for kind, details in valid.items():
            for detail in range(256):
                if detail in details:
                    d.validate_detail(kind, detail)
                else:
                    with self.assertRaises(d.ProtocolError):
                        d.validate_detail(kind, detail)

    def test_observer_silence_reset_wrap(self):
        observer = d.Observer()
        self.assertTrue(observer.report()["not_ready"])
        observer.accept(status(uptime=100), now=10)
        self.assertTrue(observer.report(now=16)["stale"])
        self.assertTrue(observer.accept(status(uptime=0), now=17)["uptime_decreased_reset_or_wrap"])
        self.assertFalse(observer.report(now=17)["stale"])
        self.assertTrue(d.Observer().report()["stale"])

    def test_observer_delayed_reception_timestamp(self):
        observer = d.Observer()
        report = observer.accept(status(age=20), now=20, received_at=10)
        self.assertEqual(report["effective_weight_age_ms"], 10020)
        self.assertTrue(report["stale"])
        self.assertTrue(report["status_observation_silent"])

    def test_repeated_cached_status_does_not_reset_freshness(self):
        observer = d.Observer()
        observer.accept(status(age=20), now=10, received_at=10)
        report = observer.accept(status(age=20), now=17, received_at=16)
        self.assertEqual(report["effective_weight_age_ms"], 7020)
        self.assertTrue(report["stale"])
        self.assertFalse(observer.accept(status(age=0, uptime=200), now=18)["stale"])

    def test_cursor_rollover_gap_reset_and_replay(self):
        cursor = d.HistoryCursor()
        self.assertTrue(cursor.observe(d.decode_metadata(snapshot(2, d.MASK)[0]))["history_replay"])
        self.assertFalse(cursor.observe(d.decode_metadata(snapshot(3, 1)[0]))["gap_or_reset"])
        self.assertTrue(cursor.observe(d.decode_metadata(snapshot(24, 100)[0]))["gap_or_reset"])
        self.assertTrue(cursor.observe(d.decode_metadata(snapshot(1, 1)[0]))["gap_or_reset"])
        self.assertTrue(d.HistoryCursor().observe(d.decode_metadata(snapshot()[0]))["history_replay"])

    def test_service_read_only(self):
        client = FakeClient()
        d.verify_services(client)
        for uuid in (d.STATUS, d.METADATA, d.HISTORY):
            char = client.services.service.chars[uuid]
            old = char.properties
            char.properties = old + ["write"]
            with self.assertRaises(d.ProtocolError):
                d.verify_services(client)
            char.properties = old
        client.services.service.chars[d.STATUS].properties = ["read", "notify"]
        with self.assertRaises(d.ProtocolError):
            d.verify_services(client)
        client.services.service = None
        with self.assertRaises(d.ProtocolError):
            d.verify_services(client)

    def test_cli_errors_and_lazy_import(self):
        script = Path(d.__file__)
        for args in (["--help"], ["invalid"], ["watch", "--reconnects", "-1"],
                     ["status", "--timeout", "nan"], ["history", "--retries", "0"],
                     ["watch", "--interval", "0.49"]):
            result = subprocess.run([sys.executable, str(script), *args], capture_output=True)
            self.assertEqual(result.returncode, 0 if args == ["--help"] else 2)
        source = "import sys;sys.path.insert(0,'tools');import shotstopper_diagnostics;assert 'bleak' not in sys.modules"
        subprocess.run([sys.executable, "-c", source], check=True)
        with patch.dict(sys.modules, {"bleak": None}), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(d.main(["show"]), 2)


class AsyncTests(unittest.IsolatedAsyncioTestCase):
    async def test_snapshot_retries(self):
        meta, history = snapshot()
        changed, _ = snapshot(3, 3)
        client = FakeClient(reads=[(d.METADATA, meta), (d.HISTORY, history),
            (d.METADATA, changed), (d.METADATA, meta), (d.HISTORY, history), (d.METADATA, meta)])
        actual, events = await d.read_snapshot(client, 2)
        self.assertEqual(actual.count, 2)
        self.assertEqual(len(events), 2)
        self.assertEqual(len(client.calls), 6)

    async def test_snapshot_bounded_error(self):
        meta, history = snapshot()
        client = FakeClient(reads=[(d.METADATA, meta), (d.HISTORY, history[:-1]), (d.METADATA, meta)] * 2)
        with self.assertRaisesRegex(d.ProtocolError, "after 2 attempts"):
            await d.read_snapshot(client, 2)
        self.assertEqual(len(client.calls), 6)

    async def test_header_and_slot_changes_retry(self):
        meta, history = snapshot()
        for index in (4, 16, 56):
            bad = bytearray(history)
            bad[index] ^= 1
            client = FakeClient(reads=[
                (d.METADATA, meta), (d.HISTORY, bad), (d.METADATA, meta),
                (d.METADATA, meta), (d.HISTORY, history), (d.METADATA, meta)])
            _, events = await d.read_snapshot(client, 2)
            self.assertEqual(len(events), 2)
            self.assertEqual(len(client.calls), 6)

    async def test_watch_protocol_error_has_no_subscription(self):
        client = FakeClient(reads=[(d.STATUS, b"bad")])
        with self.assertRaises(d.ProtocolError):
            await d.watch_connection(client, d.parser().parse_args(["watch"]))
        self.assertEqual(client.calls, [("read", d.STATUS)])

    async def test_slow_reads_preserve_request_start_age_and_latency(self):
        clock = [0.0]

        class SlowClient(DisconnectingClient):
            async def read_gatt_char(self, uuid):
                if uuid == d.STATUS:
                    clock[0] += 6
                if uuid == d.HISTORY:
                    clock[0] += 4
                return await super().read_gatt_char(uuid)

        output = io.StringIO()
        with patch.object(d, "time", SimpleNamespace(monotonic=lambda: clock[0])), \
                contextlib.redirect_stdout(output):
            client = SlowClient()
            with self.assertRaises(ConnectionError):
                await d.watch_connection(client, d.parser().parse_args(["watch"]))
        statuses = [json.loads(line) for line in output.getvalue().splitlines()
                    if "uptime_ms" in line]
        self.assertEqual([item["uptime_ms"] for item in statuses], [100])
        self.assertEqual(statuses[-1]["effective_weight_age_ms"], 10000)
        self.assertEqual(statuses[-1]["status_read_latency_ms"], 6000)
        self.assertEqual(statuses[-1]["poll_latency_ms"], 10000)
        self.assertTrue(statuses[-1]["stale"])

    async def test_polling_cadence_waits_after_completion_without_bursts(self):
        clock = [0.0]
        starts = []
        waits = []

        class Client(FakeClient):
            async def read_gatt_char(self, uuid):
                if uuid == d.STATUS:
                    starts.append(clock[0])
                    clock[0] += 0.1
                if uuid == d.HISTORY:
                    clock[0] += 3
                    if len(starts) == 3:
                        self.is_connected = False
                return await super().read_gatt_char(uuid)

        client = Client()

        async def sleep(delay):
            waits.append(delay)
            clock[0] += delay

        with patch.object(d, "time", SimpleNamespace(monotonic=lambda: clock[0])), \
                patch.object(d.asyncio, "sleep", sleep), \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(ConnectionError):
                await d.watch_connection(client, d.parser().parse_args(["watch", "--interval", "0.5"]))
        self.assertEqual(waits, [0.5, 0.5])
        for previous, current in zip(starts, starts[1:]):
            self.assertAlmostEqual(current - previous, 3.6)
        self.assertEqual(client.calls, [("read", uuid) for _ in range(3)
                         for uuid in (d.STATUS, d.METADATA, d.HISTORY, d.METADATA)])

    async def test_direct_watch_enforces_minimum_interval(self):
        args = d.parser().parse_args(["watch"])
        args.interval = 0.49
        client = FakeClient()
        with self.assertRaises(ValueError):
            await d.watch_connection(client, args)
        self.assertEqual(client.calls, [])

    async def test_one_shot_accounts_for_read_latency(self):
        clock = [0.0]

        class SlowClient(FakeClient):
            async def read_gatt_char(self, uuid):
                clock[0] += 6
                return await super().read_gatt_char(uuid)

        output = io.StringIO()
        with patch.object(d, "time", SimpleNamespace(monotonic=lambda: clock[0])), \
                contextlib.redirect_stdout(output):
            await d.run(d.parser().parse_args(["status"]), Scanner, SlowClient)
        report = json.loads(output.getvalue())
        self.assertEqual(report["status_read_latency_ms"], 6000)
        self.assertEqual(report["effective_weight_age_ms"], 6000)
        self.assertTrue(report["stale"])

    async def test_all_commands_no_application_writes(self):
        for command in ("discover", "show", "status", "history", "watch"):
            with self.subTest(command=command), contextlib.redirect_stdout(io.StringIO()):
                args = d.parser().parse_args([command])
                if command == "watch":
                    with self.assertRaises(ConnectionError):
                        await d.run(args, Scanner, DisconnectingClient)
                else:
                    await d.run(args, Scanner, FakeClient)
        for client in FakeClient.instances:
            for operation, uuid in client.calls:
                self.assertEqual(operation, "read")
                self.assertIn(uuid, (d.STATUS, d.METADATA, d.HISTORY))

    async def test_never_connect_scale_or_unmatched_address(self):
        class ScaleAdvertisement:
            local_name = "ACAIA"

        class ScaleScanner:
            @staticmethod
            async def discover(**kwargs):
                return {"scale": (Device(), ScaleAdvertisement())}

        for scanner, args in ((ScaleScanner, ["status"]),
                              (Scanner, ["status", "--address", "other"])):
            before = len(FakeClient.instances)
            with self.assertRaises(ValueError), contextlib.redirect_stdout(io.StringIO()):
                await d.run(d.parser().parse_args(args), scanner, FakeClient)
            self.assertEqual(before, len(FakeClient.instances))

    async def test_reconnect_bounded_and_replay_explicit(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), patch.object(d.asyncio, "sleep") as sleep:
            with self.assertRaises(ConnectionError):
                await d.run(d.parser().parse_args(["watch", "--reconnects", "1"]), Scanner, DisconnectingClient)
        self.assertEqual(sleep.call_count, 1)
        self.assertEqual(output.getvalue().count('"identity_continuity": "not guaranteed"'), 2)
        self.assertIn('"stale": true', output.getvalue())


if __name__ == "__main__":
    unittest.main()
