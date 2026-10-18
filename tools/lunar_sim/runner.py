#!/usr/bin/env python3
"""Run the real Micra sketch with virtual GPIO and a Mac BLE transport."""
from __future__ import annotations

import argparse
import asyncio
import ctypes
from datetime import datetime, timezone
import hashlib
import io
import json
import math
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tarfile
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
HOST = Path(__file__).resolve().parent / "host"
SOURCES = ("AcaiaArduinoBLE.cpp", "AcaiaArduinoBLE.h", "examples/shotStopper/shotStopper.ino")
RX = "49535343-1e4d-4bd9-ba61-23c647249616"
TX = "49535343-8841-43f4-a8d4-ecbe34729bb3"
OLD = "00002a80-0000-1000-8000-00805f9b34fb"
IDENTIFY = bytes.fromhex("efdd0b3031323334353637383930313233349a6d")
NOTIFICATIONS = bytes.fromhex("efdd0c0900010102020503041506")
HEARTBEAT = bytes.fromhex("efdd0002000200")
RESET = bytes.fromhex("efdd0d00010001")
START = bytes.fromhex("efdd0d00000000")
STOP = bytes.fromhex("efdd0d00020002")
TARE = bytes.fromhex("efdd04000000")
CONTROL_PACKETS = {RESET, START, STOP, TARE}
MONITOR_PACKETS = {IDENTIFY, NOTIFICATIONS, HEARTBEAT}
STOP_SETTLE_SECONDS = 0.5


def normalize_uuid(value):
    value = value.lower()
    if len(value) == 4:
        value = "0000" + value
    if len(value) == 8:
        value += "-0000-1000-8000-00805f9b34fb"
    return value


def build_native(directory, ref=None):
    directory = Path(directory)
    source = directory / "source"
    source.mkdir()
    if ref:
        commit = subprocess.check_output(
            ["git", "rev-parse", "--verify", "--end-of-options", ref + "^{commit}"],
            cwd=ROOT, text=True,
        ).strip()
        archive = subprocess.check_output(["git", "archive", commit, *SOURCES], cwd=ROOT)
        with tarfile.open(fileobj=io.BytesIO(archive)) as contents:
            contents.extractall(source, filter="data")
    else:
        commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
        for name in SOURCES:
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes((ROOT / name).read_bytes())
    manifest = {
        "source": ref or "working-tree snapshot (includes uncommitted firmware edits)",
        "base_commit": commit,
        "sha256": {name: hashlib.sha256((source / name).read_bytes()).hexdigest() for name in SOURCES},
        "adapter_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in [*sorted(HOST.iterdir()), Path(__file__)] if path.is_file()},
    }
    library = directory / "firmware.dylib"
    command = shlex.split(os.environ.get("CXX", "clang++")) + [
        "-std=c++17", "-O0", "-g", "-fno-strict-aliasing", "-fvisibility=hidden",
        "-dynamiclib", "-pthread", "-DARDUINO_ESP32S3_DEV",
        "-DSHOTSTOPPER_SKETCH=" + json.dumps(str(source / SOURCES[2])),
        "-I" + str(HOST), "-I" + str(source), str(HOST / "native.cpp"), "-o", str(library),
    ]
    subprocess.run(command, check=True, timeout=120)
    return library, manifest


class Trace:
    def __init__(self, stream, console=True):
        self.stream = stream
        self.console = console
        self.started = time.monotonic()
        self.shot_starts = 0

    def record(self, kind, **fields):
        entry = {"time": datetime.now(timezone.utc).isoformat(),
                 "elapsed_s": round(time.monotonic() - self.started, 6),
                 "event": kind, **fields}
        self.stream.write(json.dumps(entry, allow_nan=False) + "\n")
        self.stream.flush()
        if kind == "firmware" and fields.get("message") == "shot started":
            self.shot_starts += 1
        if self.console and kind in {"firmware", "transport_error", "native_error", "error",
                                     "control", "cycle_result", "run_end", "cleanup_timer_stop",
                                     "cleanup_timer_stop_written", "cleanup_incomplete", "warm_start_ready"}:
            if kind == "firmware":
                try:
                    values = fields.get("message", "").split()
                    if values and all(math.isfinite(float(value)) for value in values):
                        return  # Measurements remain in the JSONL log and status output.
                except ValueError:
                    pass
            print("[{:.3f}] {}: {}".format(entry["elapsed_s"], kind, fields), flush=True)


class Native:
    RPC = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p,
                          ctypes.c_void_p, ctypes.c_uint)
    LOG = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_uint64, ctypes.c_char_p)

    def __init__(self, library, session):
        self.session = session
        self.loop = asyncio.get_running_loop()
        self.lib = ctypes.CDLL(str(library))
        self.lib.sim_init.argtypes = [self.RPC, self.LOG]
        self.lib.sim_init.restype = ctypes.c_int
        self.lib.sim_step.restype = ctypes.c_int
        self.lib.sim_stop.restype = None
        self.lib.sim_paddle.argtypes = [ctypes.c_int]
        self.lib.sim_paddle.restype = None
        self.lib.sim_goal.argtypes = [ctypes.c_int]
        self.lib.sim_goal.restype = None
        self.lib.sim_state.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        self.lib.sim_state.restype = ctypes.c_int
        self.lib.sim_notify.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_uint]
        self.lib.sim_notify.restype = ctypes.c_int
        self.closing = threading.Event()
        self.lock = threading.Lock()
        self.pending = set()
        self.error = None
        self.rpc_callback = self.RPC(self._rpc)
        self.log_callback = self.LOG(self._log)
        self.thread = threading.Thread(target=self._run, name="native-firmware")

    def _rpc(self, operation, argument, output, capacity):
        future = None
        try:
            with self.lock:
                if self.closing.is_set():
                    raise RuntimeError("Runner is shutting down")
                future = asyncio.run_coroutine_threadsafe(
                    self.session.rpc(operation.decode(), argument.decode()), self.loop)
                self.pending.add(future)
            text = future.result(timeout=25).encode()
            if len(text) >= capacity:
                raise RuntimeError("RPC response exceeds native buffer")
            ctypes.memmove(output, text + b"\0", len(text) + 1)
            return len(text)
        except Exception as error:
            if future:
                future.cancel()
            text = (type(error).__name__ + ": " + str(error)).encode()[:capacity - 1]
            ctypes.memmove(output, text + b"\0", len(text) + 1)
            return -1
        finally:
            if future:
                with self.lock:
                    self.pending.discard(future)

    def _log(self, kind, firmware_ms, message):
        self.loop.call_soon_threadsafe(
            lambda: self.session.trace.record(kind.decode(), firmware_ms=firmware_ms,
                                              message=message.decode(errors="replace")))

    def _run(self):
        try:
            result = self.lib.sim_init(self.rpc_callback, self.log_callback)
            while result == 0 and not self.closing.is_set():
                result = self.lib.sim_step()
                time.sleep(0.001)
            if result < 0:
                self.error = "Native firmware raised an exception; see native_error log"
        except Exception as error:
            self.error = type(error).__name__ + ": " + str(error)
        finally:
            self.loop.call_soon_threadsafe(self.session.finished.set)

    def start(self):
        self.thread.start()

    def notify(self, uuid, data):
        if self.closing.is_set():
            return
        payload = ctypes.create_string_buffer(bytes(data))
        if self.lib.sim_notify(uuid.encode(), payload, len(data)) != 0:
            raise RuntimeError("Native notification queue overflow or oversized BLE value")

    def snapshot(self):
        output = ctypes.create_string_buffer(4096)
        if self.lib.sim_state(output, len(output)) < 0:
            raise RuntimeError("Native state exceeds output buffer")
        return json.loads(output.value)

    async def close(self):
        with self.lock:
            self.closing.set()
            self.lib.sim_stop()
            pending = list(self.pending)
        for future in pending:
            future.cancel()
        if self.thread.ident is not None:
            await asyncio.to_thread(self.thread.join, 5)
            if self.thread.is_alive():
                raise RuntimeError("Native firmware thread did not stop")
        await asyncio.sleep(0)


class RealBLE:
    def __init__(self, target):
        from bleak import BleakClient, BleakScanner
        self.Client, self.Scanner = BleakClient, BleakScanner
        self.target = target.lower()
        self.scanner = None
        self.device = None
        self.name = ""
        self.client = None
        self.subscriptions = set()
        self.session = None

    @property
    def connected(self):
        return self.client is not None and self.client.is_connected

    def _advertisement(self, device, advertisement):
        if device.address.lower() == self.target:
            self.device = device
            self.name = advertisement.local_name or device.name or ""

    async def handle(self, op, arg):
        if op == "SCAN":
            if arg and arg.lower() != self.target:
                raise ValueError("The sketch address does not match the selected CoreBluetooth UUID")
            if self.scanner is None:
                self.device = None
                self.scanner = self.Scanner(detection_callback=self._advertisement)
                await self.scanner.start()
            return "1"
        if op == "STOPSCAN":
            if self.scanner is not None:
                await self.scanner.stop()
                self.scanner = None
            return "1"
        if op == "AVAILABLE":
            return (self.device.address.encode().hex() + "\t" + self.name.encode().hex()
                    if self.device is not None else "")
        if op == "CONNECT":
            if arg.lower() != self.target or self.device is None:
                raise ValueError("Only the explicitly selected Lunar may be connected")
            if not self.connected:
                self.client = self.Client(self.device, disconnected_callback=self._disconnected,
                                          timeout=15)
                await self.client.connect()
                services = self.client.services
                if not ((services.get_characteristic(RX) and services.get_characteristic(TX))
                        or services.get_characteristic(OLD)):
                    await self.client.disconnect()
                    raise RuntimeError("Selected device does not expose an Acaia UART profile")
                self.session.trace.record("link_connected", name=self.name)
            return "1"
        if op == "DISCONNECT":
            if self.connected:
                await self.client.disconnect()
            return "1"
        if not self.connected:
            raise ConnectionError("Lunar is not connected")
        services = self.client.services
        if op == "DISCOVER":
            return "1"
        if op == "SERVICES":
            return "\t".join(service.uuid for service in services)
        if op == "CHARACTERS":
            service = services.get_service(arg)
            if service is None:
                raise ValueError("Unknown service")
            return "\t".join(char.uuid for char in service.characteristics)
        if op == "READ_DESCRIPTOR":
            return bytes(await self.client.read_gatt_descriptor(int(arg))).hex()
        if op == "CHAR":
            char = services.get_characteristic(normalize_uuid(arg))
            if char is None:
                return "0"
            masks = {"read": 2, "write-without-response": 4, "write": 8,
                     "notify": 16, "indicate": 32}
            return str(sum(masks.get(prop, 0) for prop in char.properties))
        if op == "SUBSCRIBE":
            if arg not in self.subscriptions:
                client = self.client
                def notify(char, data):
                    if client is self.client and self.connected:
                        self.session.receive(arg, data)
                    else:
                        self.session.trace.record("obsolete_notification", uuid=arg, hex=bytes(data).hex())
                await client.start_notify(arg, notify)
                self.subscriptions.add(arg)
            return "1"
        if op == "READ":
            return bytes(await self.client.read_gatt_char(arg)).hex()
        if op == "DESCRIPTORS":
            char = services.get_characteristic(arg)
            if char is None:
                raise ValueError("Unknown characteristic")
            return "\t".join(str(desc.handle) + ":" + desc.uuid for desc in char.descriptors)
        if op == "WRITE":
            uuid, response, payload = arg.split("\t")
            await self.client.write_gatt_char(uuid, bytes.fromhex(payload), response=response == "1")
            return "1"
        raise ValueError("Unknown BLE operation: " + op)

    def _disconnected(self, client):
        if client is self.client:
            self.subscriptions.clear()
            self.session.trace.record("link_disconnected")

    async def close(self):
        try:
            await self.handle("STOPSCAN", "")
        finally:
            if self.client is not None:
                await asyncio.wait_for(self.client.disconnect(), 5)


class FakeBLE:
    """Explicit offline fixture, not an estimate of ESP32 radio behavior."""
    def __init__(self):
        self.connected = False
        self.scanning = False
        self.subscribed = False
        self.stream = None
        self.session = None

    async def handle(self, op, arg):
        if op == "SCAN":
            self.scanning = True
            return "1"
        if op == "STOPSCAN":
            self.scanning = False
            return "1"
        if op == "AVAILABLE":
            return "46414b45\t4c554e41522d53494d" if self.scanning and not self.connected else ""
        if op == "CONNECT":
            self.connected = True
            return "1"
        if op == "DISCONNECT":
            self.connected = False
            self.subscribed = False
            return "1"
        if not self.connected:
            raise ConnectionError("Fake Lunar disconnected")
        if op == "CHAR":
            return str(24 if arg == RX else 12 if arg == TX else 0)
        if op == "SUBSCRIBE":
            self.subscribed = True
            return "1"
        if op == "DISCOVER":
            return "1"
        if op == "WRITE":
            payload = bytes.fromhex(arg.split("\t")[2])
            if payload == NOTIFICATIONS and self.stream is None:
                self.stream = asyncio.create_task(self._weights())
            return "1"
        raise ValueError("Unsupported fake operation: " + op)

    async def _weights(self):
        while True:
            await asyncio.sleep(0.125)
            if self.connected and self.subscribed:
                self.session.receive(RX, bytes.fromhex("efdd0c08050000000001000905"))

    async def close(self):
        self.connected = False
        if self.stream:
            self.stream.cancel()
            try:
                await self.stream
            except asyncio.CancelledError:
                pass


class Session:
    def __init__(self, backend, trace, allow_commands=False, fragment_size=0):
        self.backend, self.trace = backend, trace
        backend.session = self
        self.allow_commands = allow_commands
        self.fragment_size = fragment_size
        self.drop_notifications = False
        self.native = None
        self.finished = asyncio.Event()
        self.error = None
        self.raw_count = 0
        self.saw_weight = False
        self.timer_started = False
        self.last_write = 0.0
        self.last_stop = None
        self.write_uuid = TX
        self.monitor_task = None

    async def rpc(self, operation, argument):
        if operation == "WRITE":
            fields = argument.split("\t")
            if len(fields) != 3 or fields[1] not in ("0", "1"):
                raise ValueError("Invalid write request")
            uuid, response, payload = fields
            packet = bytes.fromhex(payload)
            if uuid not in (TX, OLD):
                raise PermissionError("Writes are restricted to the Acaia command characteristic")
            if packet not in MONITOR_PACKETS and packet not in CONTROL_PACKETS:
                raise PermissionError("Unrecognized scale command")
            if packet in CONTROL_PACKETS and not self.allow_commands:
                raise PermissionError("Tare/timer commands require --allow-scale-commands")
            self.write_uuid = uuid
            if packet == START:
                self.timer_started = True
            self.trace.record("write_requested", uuid=uuid, hex=payload, response=response == "1")
            result = await asyncio.wait_for(self.backend.handle(operation, argument), 20)
            self.last_write = time.monotonic()
            if packet == STOP:
                self.timer_started = False
                self.last_stop = self.last_write
            self.trace.record("write_completed", hex=payload, meaning="transport completion, not scale execution")
            return result
        lifecycle = operation in {"SCAN", "STOPSCAN", "CONNECT", "DISCONNECT", "DISCOVER", "SUBSCRIBE"}
        if lifecycle:
            self.trace.record("ble_operation", operation=operation, argument=argument, phase="requested")
        try:
            result = await asyncio.wait_for(self.backend.handle(operation, argument), 20)
        except Exception as error:
            self.trace.record("ble_operation_error", operation=operation, message=str(error))
            raise
        if lifecycle:
            self.trace.record("ble_operation", operation=operation, phase="completed")
        return result

    def receive(self, uuid, data):
        self.raw_count += 1
        delivered = (not self.drop_notifications and self.native is not None
                     and not self.native.closing.is_set())
        self.trace.record("notification", uuid=uuid, hex=bytes(data).hex(),
                          delivered=delivered, fragment_size=self.fragment_size)
        if not delivered:
            return
        try:
            size = self.fragment_size or len(data) or 1
            for offset in range(0, len(data), size):
                self.native.notify(uuid, data[offset:offset + size])
        except Exception as error:
            self.error = str(error)
            self.trace.record("error", message=self.error)
            self.finished.set()

    async def start(self, library):
        self.native = Native(library, self)
        self.native.start()
        self.monitor_task = asyncio.create_task(self._monitor())

    async def _monitor(self):
        previous = None
        last_log = 0.0
        try:
            while not self.finished.is_set():
                snapshot = self.native.snapshot()
                self.saw_weight |= snapshot.get("last_weight_ms", 0) > 0
                key = tuple(snapshot.get(name) for name in (
                    "connected", "brewing", "latched", "paddle", "relay", "weight", "last_weight_ms"))
                if key != previous or time.monotonic() - last_log > 0.5:
                    self.trace.record("state", state=snapshot)
                    previous, last_log = key, time.monotonic()
                await asyncio.sleep(0.02)
        except Exception as error:
            self.error = str(error)
            self.trace.record("error", message=self.error)
            self.finished.set()

    def control(self, command):
        words = command.lower().split()
        if words == ["quit"]:
            self.finished.set()
            return
        if words == ["status"]:
            print(json.dumps(self.native.snapshot(), indent=2), flush=True)
            return
        if not self.allow_commands:
            raise PermissionError("Controls and fault injection require --allow-scale-commands")
        if len(words) == 2 and words[0] in ("paddle", "drop") and words[1] in ("on", "off"):
            on = words[1] == "on"
            if words[0] == "paddle":
                self.native.lib.sim_paddle(on)
            else:
                self.drop_notifications = on
        elif len(words) == 2 and words[0] == "goal" and words[1].isdigit():
            grams = int(words[1])
            if not 10 <= grams <= 200:
                raise ValueError("Goal must be 10..200 grams")
            self.native.lib.sim_goal(grams)
        else:
            raise ValueError("Commands: status, quit, paddle on/off, drop on/off, goal 10..200")
        self.trace.record("control", command=command)

    async def cycle(self, wait_for_weights=False):
        previous_weight_ms = 0
        consecutive_weights = 0
        while True:
            if self.finished.is_set():
                raise RuntimeError("Runner stopped before connecting")
            state = self.native.snapshot()
            stamp = state.get("last_weight_ms", 0)
            if not state.get("connected") or not stamp:
                previous_weight_ms = 0
                consecutive_weights = 0
            elif stamp != previous_weight_ms:
                consecutive_weights = (consecutive_weights + 1
                                       if 0 < stamp - previous_weight_ms <= 1000 else 1)
                previous_weight_ms = stamp
            if state.get("connected") and (not wait_for_weights or consecutive_weights >= 3):
                break
            await asyncio.sleep(0.02)
        if wait_for_weights:
            self.trace.record("warm_start_ready", decoded_updates=consecutive_weights, state=state)
        starts = self.trace.shot_starts
        self.control("paddle on")
        await asyncio.sleep(6)
        takeover = self.native.snapshot().get("latched", False)
        self.control("paddle off")
        await asyncio.sleep(0.4)
        state = self.native.snapshot()
        result = {"takeover": takeover, "held_after_release": state.get("relay") == 1,
                  "shot_starts": self.trace.shot_starts - starts,
                  "start_condition": "decoded_weights" if wait_for_weights else "connected",
                  "note": "Audible beeps and physical command execution require observation"}
        self.trace.record("cycle_result", **result)
        return result["takeover"] and result["held_after_release"] and result["shot_starts"] == 1

    async def close(self):
        self.finished.set()
        if self.monitor_task:
            self.monitor_task.cancel()
            try:
                await self.monitor_task
            except asyncio.CancelledError:
                pass
        try:
            if self.native:
                await self.native.close()
        finally:
            try:
                if self.timer_started and self.allow_commands:
                    if not self.backend.connected:
                        message = "Cannot confirm timer stop: link is down; stop the scale timer manually if needed"
                        self.trace.record("cleanup_incomplete", message=message)
                        raise RuntimeError(message)
                    properties = int(await asyncio.wait_for(
                        self.backend.handle("CHAR", self.write_uuid), 5))
                    if not properties & (8 | 4):
                        raise RuntimeError("Cannot send timer stop: command characteristic is not writable")
                    response = bool(properties & 8)
                    await asyncio.sleep(max(0, 0.1 - (time.monotonic() - self.last_write)))
                    self.trace.record("cleanup_timer_stop", response=response,
                                      meaning="outside firmware; trial ending")
                    await asyncio.wait_for(self.backend.handle(
                        "WRITE", self.write_uuid + ("\t1\t" if response else "\t0\t") + STOP.hex()), 5)
                    self.timer_started = False
                    self.last_stop = time.monotonic()
                    self.trace.record("cleanup_timer_stop_written", response=response,
                                      application_execution="still requires observation")
                if self.last_stop is not None and self.backend.connected:
                    # Keep the link alive for queued STOP writes and application processing.
                    await asyncio.sleep(max(0, STOP_SETTLE_SECONDS - (time.monotonic() - self.last_stop)))
            finally:
                await asyncio.wait_for(self.backend.close(), 10)
        if self.native and self.native.error:
            raise RuntimeError(self.native.error)
        if self.error:
            raise RuntimeError(self.error)


async def discover():
    from bleak import BleakScanner
    devices = await BleakScanner.discover(timeout=12, return_adv=True)
    matches = [{"name": adv.local_name or device.name, "id": device.address}
               for device, adv in devices.values()
               if (adv.local_name or device.name or "").upper().startswith(("LUNAR", "ACAIA"))]
    print(json.dumps(matches, indent=2))
    return bool(matches)


async def run(args, library, manifest, trace):
    backend = FakeBLE() if args.fake else RealBLE(args.device)
    session = Session(backend, trace, args.allow_scale_commands, args.fragment_size)
    trace.record("manifest", **manifest, backend="FAKE" if args.fake else "CoreBluetooth",
                 device=args.device, controls_enabled=args.allow_scale_commands,
                 fragment_size=args.fragment_size,
                 start_condition="decoded_weights" if getattr(args, "start_after_weights", False) else "connected")
    loop = asyncio.get_running_loop()
    reader = False
    installed_signals = []
    success = False
    cycle_task = None
    stop_task = None
    try:
        await session.start(library)
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(sig, session.finished.set)
            installed_signals.append(sig)
        if args.interactive:
            def read_command():
                line = sys.stdin.readline()
                try:
                    session.control(line.strip() if line else "quit")
                except (ValueError, PermissionError) as error:
                    print(str(error), file=sys.stderr, flush=True)
                except Exception as error:
                    session.error = str(error)
                    session.finished.set()
            loop.add_reader(sys.stdin.fileno(), read_command)
            reader = True
            print("Commands: status, quit, paddle on/off, drop on/off, goal 10..200", flush=True)
        if args.scenario == "cycle":
            cycle_task = asyncio.create_task(
                session.cycle(wait_for_weights=getattr(args, "start_after_weights", False)))
            stop_task = asyncio.create_task(session.finished.wait())
            done, _ = await asyncio.wait({cycle_task, stop_task}, timeout=args.seconds,
                                         return_when=asyncio.FIRST_COMPLETED)
            if cycle_task in done:
                success = cycle_task.result()
            elif not done:
                raise TimeoutError("Cycle did not complete within the time limit")
        else:
            try:
                await asyncio.wait_for(session.finished.wait(), args.seconds)
            except asyncio.TimeoutError:
                pass
            success = session.saw_weight
        return success
    finally:
        if reader:
            loop.remove_reader(sys.stdin.fileno())
        tasks = [task for task in (cycle_task, stop_task) if task is not None]
        for task in tasks:
            task.cancel()
        results = await asyncio.gather(*tasks, return_exceptions=True)
        try:
            await session.close()
        finally:
            for sig in installed_signals:
                loop.remove_signal_handler(sig)
        for result in results:
            if isinstance(result, Exception):
                raise result
        trace.record("run_end", success=success, notifications=session.raw_count,
                     decoded_weights_observed=session.saw_weight)


def main():
    if sys.version_info < (3, 12):
        raise SystemExit("Use Python 3.12 or newer for safe Git archive extraction and the BLE backend")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref", help="Git ref to export; default is a snapshot of working-tree firmware")
    parser.add_argument("--scan", action="store_true")
    parser.add_argument("--device", help="Exact CoreBluetooth device UUID from --scan")
    parser.add_argument("--fake", action="store_true", help="Explicit offline fake BLE backend")
    parser.add_argument("--log", type=Path, help="New JSONL log path; keep outside the repository")
    parser.add_argument("--allow-scale-commands", action="store_true")
    parser.add_argument("--interactive", action="store_true")
    parser.add_argument("--scenario", choices=("monitor", "cycle"), default="monitor")
    parser.add_argument("--start-after-weights", action="store_true",
                        help="Warm-start cycle: wait for three decoded updates before virtual paddle-on")
    parser.add_argument("--seconds", type=float, default=120)
    parser.add_argument("--fragment-size", type=int, default=0, help="Explicit synthetic fragmentation, not observed radio behavior")
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600 or not 0 <= args.fragment_size <= 512:
        parser.error("seconds must be 1..3600 and fragment-size 0..512")
    if args.scan:
        return 0 if asyncio.run(discover()) else 2
    if not args.log or (not args.fake and not args.device):
        parser.error("Provide --log and either --device or --fake")
    if args.fake and args.device:
        parser.error("--fake and --device are mutually exclusive")
    if (args.scenario == "cycle" or args.fragment_size) and not args.allow_scale_commands:
        parser.error("A cycle or fault injection requires --allow-scale-commands")
    if args.start_after_weights and args.scenario != "cycle":
        parser.error("--start-after-weights requires --scenario cycle")
    with args.log.open("x", encoding="utf-8") as stream:
        trace = Trace(stream)
        try:
            with tempfile.TemporaryDirectory(prefix="lunar-native-") as directory:
                library, manifest = build_native(directory, args.ref)
                print("FAKE BLE" if args.fake else "LIVE LUNAR", manifest["source"], flush=True)
                print("Monitor mode" if not args.allow_scale_commands else "Tare/timer commands ENABLED", flush=True)
                return 0 if asyncio.run(run(args, library, manifest, trace)) else 2
        except Exception as error:
            trace.record("error", message=type(error).__name__ + ": " + str(error))
            print(str(error), file=sys.stderr)
            return 1


if __name__ == "__main__":
    sys.exit(main())
