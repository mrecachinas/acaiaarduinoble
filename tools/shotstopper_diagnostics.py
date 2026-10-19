#!/usr/bin/env python3
"""Read-only ShotStopper diagnostics; importing this module never imports Bleak."""

import argparse
import asyncio
from dataclasses import asdict, dataclass
import json
import struct
import sys
import time

SERVICE = "7b183000-6e2d-4a9b-9c51-4c5344470001"
STATUS = "7b183001-6e2d-4a9b-9c51-4c5344470001"
METADATA = "7b183002-6e2d-4a9b-9c51-4c5344470001"
HISTORY = "7b183003-6e2d-4a9b-9c51-4c5344470001"
CAPACITY = 24
MASK = 0xFFFFFFFF
UNKNOWN_AGE = MASK
EVENT_NAMES = {
    1: "connection_attempt", 2: "connection_ready", 3: "connection_failed",
    4: "disconnected", 5: "timeout", 6: "shot_start", 7: "shot_end",
    8: "latch", 9: "command_result", 10: "learning_wait",
    11: "learning_rejected", 12: "learning_update", 13: "boot",
}


class ProtocolError(ValueError):
    pass


class SnapshotChanged(Exception):
    pass


def unpack(data, fmt, size, packet_type):
    if len(data) != size:
        raise ProtocolError(f"type {packet_type}: expected {size} bytes, got {len(data)}")
    if data[0] != 1 or data[1] != packet_type:
        raise ProtocolError(f"unsupported version/type {data[0]}/{data[1]}")
    return struct.unpack(fmt, data)


@dataclass(frozen=True)
class Status:
    flags: int
    uptime_ms: int
    weight_decigrams: int
    target_decigrams: int
    actual_offset_decigrams: int
    weight_age_ms: int
    shot_elapsed_deciseconds: int

    def report(self, silence_ms=0):
        result = asdict(self)
        connected = bool(self.flags & 1)
        valid = bool(self.flags & 2)
        age = None if self.weight_age_ms == UNKNOWN_AGE else self.weight_age_ms + silence_ms
        result.update(
            connected=connected, accepted_weight_valid=valid,
            effective_weight_age_ms=age,
            not_ready=not connected or not valid or age is None,
            stale=not connected or age is None or age > 5000,
            status_observation_silent=silence_ms > 5000,
        )
        return result


def decode_status(data):
    values = unpack(data, "<BBHIhHhIH", 20, 1)
    if values[2] & ~0x7F:
        raise ProtocolError("reserved status flags are nonzero")
    if bool(values[2] & 2) != (values[7] != UNKNOWN_AGE):
        raise ProtocolError("weight-valid flag and sample age disagree")
    return Status(*values[2:])


@dataclass(frozen=True)
class Metadata:
    capacity: int
    count: int
    generation: int
    oldest: int
    next: int
    overwritten: int


def decode_metadata(data):
    values = unpack(data, "<BBBBIIII", 20, 2)
    meta = Metadata(*values[2:])
    if meta.capacity != CAPACITY or meta.count > CAPACITY:
        raise ProtocolError("invalid metadata capacity/count")
    if meta.oldest != (meta.next - meta.count) & MASK:
        raise ProtocolError("invalid metadata sequence range")
    return meta


@dataclass(frozen=True)
class Event:
    seq: int
    timestamp_ms: int
    kind: int
    detail: int
    actual_offset_decigrams: int
    value_decigrams: int
    sample_age_ms: int

    def report(self):
        return dict(asdict(self), event=EVENT_NAMES[self.kind])


def validate_detail(kind, detail):
    if kind not in EVENT_NAMES:
        raise ProtocolError(f"unknown event kind {kind}")
    limits = {1: 0, 2: 3, 3: 8, 4: 1, 5: 0, 6: 0, 7: 4,
              8: 1, 10: 1, 11: 0, 12: 0, 13: 0}
    if kind == 9:
        command = detail & 0x7F
        valid = 1 <= command <= 7 and not (command == 7 and detail & 0x80)
    else:
        valid = detail <= limits[kind] and not (kind == 3 and detail == 0)
    if not valid:
        raise ProtocolError(f"invalid detail {detail} for event {kind}")


def decode_history(data, meta):
    if len(data) != 496:
        raise ProtocolError(f"history: expected 496 bytes, got {len(data)}")
    _, _, count, generation, oldest, next_seq = unpack(data[:16], "<BBHIII", 16, 3)
    if (count, generation, oldest, next_seq) != (
            meta.count, meta.generation, meta.oldest, meta.next):
        raise ProtocolError("history header differs from metadata")
    events = []
    for index in range(CAPACITY):
        slot = data[16 + index * 20:36 + index * 20]
        if index >= count:
            if any(slot):
                raise ProtocolError("nonzero unused history slot")
            continue
        event = Event(*struct.unpack("<IIBBhiI", slot))
        if event.seq != (oldest + index) & MASK:
            raise ProtocolError("history slot sequence mismatch")
        validate_detail(event.kind, event.detail)
        events.append(event)
    return events


async def read_snapshot(client, retries=3):
    if retries < 1:
        raise ValueError("retries must be positive")
    for _ in range(retries):
        before = decode_metadata(await client.read_gatt_char(METADATA))
        data = await client.read_gatt_char(HISTORY)
        after = decode_metadata(await client.read_gatt_char(METADATA))
        if before != after:
            continue
        return after, decode_history(data, after)
    raise SnapshotChanged(f"history changed during all {retries} attempts")


def verify_services(client):
    service = client.services.get_service(SERVICE)
    if service is None:
        raise ProtocolError("selected shotStopper does not expose diagnostics service")
    for uuid, expected in ((STATUS, {"read"}), (METADATA, {"read"}), (HISTORY, {"read"})):
        characteristic = service.get_characteristic(uuid)
        if characteristic is None or set(characteristic.properties) != expected:
            raise ProtocolError(f"unexpected/missing read-only characteristic {uuid}")


def is_shotstopper(device, advertisement):
    name = advertisement.local_name or device.name or ""
    return name.casefold() == "shotstopper"


async def discover(scanner, timeout):
    found = await scanner.discover(timeout=timeout, return_adv=True)
    return [device for device, adv in found.values() if is_shotstopper(device, adv)]


def emit(value):
    print(json.dumps(value, sort_keys=True), flush=True)


class Observer:
    """Never deduplicate history across connections: identity/boot are not durable."""

    def __init__(self):
        self.status = None
        self.received_at = None

    def accept(self, data, now=None, received_at=None):
        status = decode_status(data)
        reset = self.status is not None and status.uptime_ms < self.status.uptime_ms
        now = time.monotonic() if now is None else now
        received_at = now if received_at is None else received_at
        if status == self.status and self.received_at is not None:
            received_at = min(received_at, self.received_at)
        self.status = status
        self.received_at = received_at
        return dict(self.report(now), uptime_decreased_reset_or_wrap=reset)

    def report(self, now=None):
        if self.status is None:
            return {"not_ready": True, "stale": True, "reason": "no status received"}
        now = time.monotonic() if now is None else now
        return self.status.report(max(0, int((now - self.received_at) * 1000)))


class HistoryCursor:
    def __init__(self):
        self.next = None

    def observe(self, meta):
        result = {"history_replay": self.next is None, "gap_or_reset": False}
        if self.next is not None:
            advance = (meta.next - self.next) & MASK
            if advance > meta.count:
                result.update(gap_or_reset=True,
                              reason="overrun, reset, or ambiguous sequence discontinuity",
                              modular_gap=advance - meta.count)
        self.next = meta.next
        return result


async def watch_connection(client, args):
    observer = Observer()
    cursor = HistoryCursor()
    if args.interval < 0.5:
        raise ValueError("watch interval must be at least 0.5 seconds")
    while client.is_connected:
        read_started_at = time.monotonic()
        accepted = observer.accept(await client.read_gatt_char(STATUS),
                                   received_at=read_started_at)
        read_completed_at = time.monotonic()
        try:
            meta, events = await read_snapshot(client, args.retries)
        except SnapshotChanged as error:
            history_report = {"snapshot_inconsistent": True, "error": str(error)}
        else:
            history_report = dict(cursor.observe(meta), snapshot_replayed=True,
                                  metadata=asdict(meta),
                                  events=[event.report() for event in events])
        emit(dict(observer.report(),
                  uptime_decreased_reset_or_wrap=accepted["uptime_decreased_reset_or_wrap"],
                  status_read_latency_ms=max(0, int((read_completed_at - read_started_at) * 1000)),
                  poll_latency_ms=max(0, int((time.monotonic() - read_started_at) * 1000))))
        emit(history_report)
        if client.is_connected:
            # Wait after completion: delayed reads never trigger catch-up bursts.
            await asyncio.sleep(args.interval)
    raise ConnectionError("debugger disconnected; displayed status is stale/not-ready")


async def run(args, scanner, client_factory):
    if args.command == "discover":
        devices = await discover(scanner, args.timeout)
        for device in devices:
            emit({"name": device.name, "address": device.address,
                  "verified_diagnostics": False})
        return
    for attempt in range(args.reconnects + 1):
        try:
            devices = await discover(scanner, args.timeout)
            if args.address:
                devices = [d for d in devices if d.address.casefold() == args.address.casefold()]
            if not devices:
                raise ConnectionError("no matching shotStopper found during discovery")
            if len(devices) != 1:
                raise ValueError(f"expected one named shotStopper, found {len(devices)}; use --address")
            async with client_factory(devices[0]) as client:
                verify_services(client)
                if args.command in ("show", "status"):
                    started_at = time.monotonic()
                    status = decode_status(await client.read_gatt_char(STATUS))
                    latency_ms = max(0, int((time.monotonic() - started_at) * 1000))
                    emit(dict(status.report(latency_ms), status_read_latency_ms=latency_ms))
                elif args.command == "history":
                    meta, events = await read_snapshot(client, args.retries)
                    emit({"history_replay": True, "metadata": asdict(meta),
                          "events": [event.report() for event in events]})
                else:
                    emit({"connection_attempt": attempt + 1, "history_replay": True,
                          "identity_continuity": "not guaranteed"})
                    await watch_connection(client, args)
                return
        except (ProtocolError, ValueError) as error:
            emit({"stale": True, "not_ready": True, "error": str(error)})
            raise
        except Exception as error:
            emit({"stale": True, "not_ready": True, "error": str(error)})
            if args.command != "watch" or attempt == args.reconnects:
                raise
            await asyncio.sleep(args.reconnect_delay)


def positive(value):
    number = float(value)
    if not 0 < number < float("inf"):
        raise argparse.ArgumentTypeError("must be finite and positive")
    return number


def nonnegative(value):
    number = int(value)
    if number < 0:
        raise argparse.ArgumentTypeError("must be nonnegative")
    return number


def poll_interval(value):
    number = positive(value)
    if number < 0.5:
        raise argparse.ArgumentTypeError("must be at least 0.5 seconds")
    return number


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("command", choices=("discover", "show", "status", "history", "watch"))
    result.add_argument("--address", help="select an address/identifier from named discovery (not persistent identity)")
    result.add_argument("--timeout", type=positive, default=5.0)
    result.add_argument("--interval", type=poll_interval, default=2.0,
                        help="watch delay after each completed poll (minimum 0.5 seconds)")
    result.add_argument("--retries", type=int, choices=range(1, 11), default=3)
    result.add_argument("--reconnects", type=nonnegative, default=0, help="bounded watch reconnects; default none")
    result.add_argument("--reconnect-delay", type=positive, default=5.0)
    return result


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        from bleak import BleakClient, BleakScanner
        asyncio.run(run(args, BleakScanner, BleakClient))
    except ImportError:
        print("error: install tools/requirements.txt in an isolated virtual environment", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 130
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
