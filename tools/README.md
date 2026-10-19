# Read-only Mac BLE diagnostics

`shotstopper_diagnostics.py` reads the **shotStopper**, never the scale. Python
3.9+ and Bleak are needed only for live use. An initial **wired firmware flash**
of the updated shotStopper example is necessary; this client cannot install
firmware. Do not flash or operate espresso hardware as part of offline testing.

## Isolated installation and optional live use

Run from the repository root:

```sh
python3 -m venv .venv-diagnostics
.venv-diagnostics/bin/python -m pip install -r tools/requirements.txt
.venv-diagnostics/bin/python tools/shotstopper_diagnostics.py --help
```

The following are **optional live Bluetooth operations**, not offline tests.
They require a compatible Mac/Bluetooth environment and user-granted Bluetooth
permission; no system settings are changed by the client:

```sh
.venv-diagnostics/bin/python tools/shotstopper_diagnostics.py discover
.venv-diagnostics/bin/python tools/shotstopper_diagnostics.py show
.venv-diagnostics/bin/python tools/shotstopper_diagnostics.py status
.venv-diagnostics/bin/python tools/shotstopper_diagnostics.py history
.venv-diagnostics/bin/python tools/shotstopper_diagnostics.py watch
```

`show` and `status` are aliases. Output is newline-delimited JSON. With multiple
named devices, use `--address IDENTIFIER` from discovery. Discovery filters the
local/device name `shotStopper` (case insensitive); it does not require the
diagnostics UUID in advertising. It never intentionally connects to a named
scale. A name is not authentication: a different device could impersonate it.
The diagnostics service and exact read-only properties are checked after
connecting. The existing advertised `0x0FFE` / read-write `0xFF11` goal service
remains unchanged and is **never accessed**.

There are no application characteristic writes, goal changes, tare, timer,
relay, latch, OTA, Wi-Fi, or cloud controls. All three diagnostics characteristics
are **Read only**: no Notify property, subscriptions, or CCCD writes.
`watch` polls status followed by metadata → history → metadata. It waits
`--interval` seconds after each completed poll (default 2, minimum 0.5), so
there are at most two polling cycles per second and no delayed catch-up bursts.
Snapshot retries remain bounded as described below.

`watch` exits on disconnect by default. Opt in to bounded reconnects using
`--reconnects 2 --reconnect-delay 5`. Each attempt rediscovers the named device
and verifies the service. A discovery window with no matching device consumes
an attempt and retries after the same delay while attempts remain. Multiple
matching devices and protocol errors are fatal, not retried. Neither a Mac
identifier nor a sequence number
guarantees persistent device/boot identity. Every connection flags history
replay and discards the old cursor. Full retained snapshots may be repeated.
Within a connection, modular cursor comparison reports an overrun/reset/gap
ambiguity, not a claim of continuity. Uptime decreases mean reset **or wrap**.
No history survives reboot; none is persisted by this client.

`connected` means exactly the library's scale connection state, **not** fresh
data or guaranteed physical connectivity. Physical-disconnect events are
observational: the library flag can remain true until the existing timeout or
host-failure path clears it, preserving prior brew/reconnect timing.
`accepted_weight_valid` means a supported weight was accepted since the latest
connection attempt. `not_ready` is explicit when disconnected, invalid, or age
unknown. `stale` is explicit when disconnected, age unknown, or age >5,000 ms;
watch adds elapsed local time since the status **request started** to sample
age. This conservative timestamp includes blocked status/history reads instead
of making an old snapshot appear fresh on completion. JSON reports explicit
read timing; repeated byte-identical cached status keeps its original
observation timestamp rather than resetting freshness. Timing fields include
`status_read_latency_ms`, total `poll_latency_ms`, and
`status_observation_silent` (>5 seconds since the status request began).
One-shot `show`/`status` also account for status read latency. Until another
poll completes, previously displayed data keeps aging; output is not a live
freshness guarantee while a backend read is blocked.
Any connection failure invalidates the displayed state. Values may still be
displayed for diagnosis but are not safe control inputs. A successful GATT
command write in event history does **not** prove scale execution/acknowledgment.

## Version 1 wire contract

All integers are explicitly **little-endian**, packed without native alignment.
Signed integers are two's complement. `dg` is 0.1 gram; `ms` is milliseconds.
UUIDs:

| Item | UUID | GATT properties |
|---|---|---|
| service | `7b183000-6e2d-4a9b-9c51-4c5344470001` | — |
| status | `7b183001-6e2d-4a9b-9c51-4c5344470001` | Read |
| metadata | `7b183002-6e2d-4a9b-9c51-4c5344470001` | Read |
| history | `7b183003-6e2d-4a9b-9c51-4c5344470001` | Read |

### Status: exactly 20 bytes, Python `<BBHIhHhIH`

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | version = 1 |
| 1 | u8 | type = 1 |
| 2 | u16 | flags |
| 4 | u32 | uptime_ms (wraps modulo 2^32) |
| 8 | i16 | weight_decigrams |
| 10 | u16 | target_decigrams |
| 12 | i16 | actual_offset_decigrams |
| 14 | u32 | weight_age_ms |
| 18 | u16 | shot_elapsed_deciseconds (0.1 second) |

Flags: bit 0 library scale connected; 1 accepted weight valid since latest
connection attempt; 2 brewing; 3 latched; 4 physical button pressed; 5 learning
pending; 6 range saturated. Bits 7–15 must be zero. Weight and actual offset
are clamped to i16, target to u16; bit 6 marks range saturation. Unknown sample
age is `0xFFFFFFFF`; finite age saturates at `0xFFFFFFFE`, not unknown. Elapsed
shot time saturates at 65,535 deciseconds. Unready status can contain old values.
Bit 1 is set if and only if sample age is not `0xFFFFFFFF`; inconsistent payloads
are rejected. A disconnect alone need not clear bit 1: finite age then continues
to age while the connection flag makes the status stale/not-ready.

### Metadata: exactly 20 bytes, Python `<BBBBIIII`

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | version = 1 |
| 1 | u8 | type = 2 |
| 2 | u8 | capacity = 24 |
| 3 | u8 | count (0–24) |
| 4 | u32 | generation |
| 8 | u32 | oldest sequence |
| 12 | u32 | next sequence |
| 16 | u32 | overwritten count (saturates at `0xFFFFFFFF`) |

### History: exactly 496 bytes

Header: 16 bytes, Python `<BBHIII`: version u8 = 1, type u8 = 3, count u16,
generation u32, oldest u32, next u32. Followed by exactly 24 slots of 20 bytes,
Python `<IIBBhiI`:

| Slot offset | Type | Field |
|---|---|---|
| 0 | u32 | sequence |
| 4 | u32 | timestamp_ms (uptime, wraps modulo 2^32) |
| 8 | u8 | event kind |
| 9 | u8 | detail |
| 10 | i16 | actual current offset, dg |
| 12 | i32 | last accepted scale weight, dg, or 0 if no accepted sample |
| 16 | u32 | sample_age_ms; unknown `0xFFFFFFFF`, finite saturation `0xFFFFFFFE` |

Used slots are oldest-first. All unused slots, including every byte, are zero.
Sequence starts at 0, wraps modulo 2^32, and `oldest = next - count` modulo
2^32. Generation is a wrapping history publication counter, not a persistent boot ID.
Capacity is 24 **events**, not shots. Old entries are overwritten; retained ring
RAM survives debugger disconnection, **not reboot/power loss**.

There is no application paging command. The entire 496-byte value is read via
the backend's normal ATT long-read support; MTU/read fragmentation and updates
can produce a torn snapshot. Client reads metadata → history → metadata,
requires both metadata values to match in full, requires the history header to
match, and validates contiguous modular slot sequences and zero unused padding.
Changed metadata causes a retry, up to `--retries` total attempts (default 3,
maximum 10). If every attempt races a publication, one-shot `history` fails
explicitly; `watch` emits `snapshot_inconsistent` with an error, still reports
status with elapsed read latency, and waits the normal poll interval before
trying again. No partial events are returned, and the history cursor does not
advance on failed snapshots, so later gaps remain visible.
Malformed metadata or an invalid history under unchanged metadata is a fatal
protocol error, not a recoverable race: unknown version/type/event, reserved
flags, invalid lengths/count/capacity/detail/ranges/padding remain errors.
No snapshot or cursor algorithm can prove continuity after an entire counter
wrap, reboot, or device replacement.

### Event kinds and detail

| Kind | Name | Allowed detail |
|---|---|---|
| 1 | connection_attempt | 0 |
| 2 | connection_ready | scale type 0–3 |
| 3 | connection_failed | 1 scan, 2 connect, 3 discovery, 4 type, 5 subscription, 6 identify, 7 notify request, 8 mode |
| 4 | disconnected | 0 physical observation, 1 host failure |
| 5 | timeout | 0 |
| 6 | shot_start | 0 |
| 7 | shot_end | 0 BUTTON, 1 WEIGHT, 2 TIME, 3 DISCONNECT, 4 UNDEF |
| 8 | latch | 1 assert, 0 release |
| 9 | command_result | low 7 bits: 1 tare, 2 start timer, 3 stop timer, 4 reset timer, 5 tare/start, 6 beep, 7 heartbeat; bit 7 success; heartbeat failures only |
| 10 | learning_wait | 0 drip wait, 1 below existing threshold (transitions only) |
| 11 | learning_rejected | 0 existing MAX_OFFSET |
| 12 | learning_update | 0 |
| 13 | boot | 0 |

Connection events describe transitions, not every periodic retry. An outage
retains its first `connection_attempt` and `connection_failed`; further attempts
and repeated failures at the same stage are coalesced. A changed failure stage
and successful `connection_ready` are still recorded. After a ready connection,
only the first disconnect observation (physical or host failure) is recorded
until the next ready connection. Later timeout and command-result events remain
available without falsely implying additional disconnects. These filters affect
only history: they never change scale commands, retry timing, or connection state.
Coalesced retries do not consume sequence numbers or increment the overwritten
count, and the history is not a retry counter. An unchanged outage therefore
does not continually overwrite earlier shot events or republish history.

Firmware refreshes local characteristic values at most 2 Hz; history is
serialized/refreshed only when dirty. Valid initial values are populated before
advertising. Reads may therefore lag an event. This is diagnostic telemetry, not a
hard real-time or durable audit log.

## Hardware-free tests

No Bleak installation, Bluetooth access, network, Arduino board, or flashing:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -v
```

Tests use fake scanners/clients and portable native C++ checks when a C++
compiler is available (native test skips explicitly if unavailable). They check
wire bytes, ring limits, snapshots, rollover, malformed packets, stale state,
read-only properties, absence of subscriptions/writes, polling cadence and
read latency, reconnect/replay, and CLI errors.
Regression cases cover temporary discovery misses, bounded reconnect exhaustion,
snapshot races followed by recovery, fatal malformed snapshots, and retained
shot history through an hour of simulated identical connection failures.
The native sketch harness includes the actual `.ino` and real library header,
then substitutes only Arduino GPIO/time, EEPROM, BLE transport recording, and
scale method boundaries. It executes real `setup`, `refreshDiagnostics`, and
disconnect handlers to check GATT permissions/sizes, 500 ms status publication
(including timer wrap), dirty-only history, retained ring, and no added
scale-command/EEPROM/GPIO/disconnect calls when the debugger disconnects.
It does **not** compile the library implementation or prove controller behavior.

The harness asserts the real sketch declares **no Notify or Write property**
on any diagnostics characteristic. It does not simulate the ArduinoBLE
notification gate as proof of delivery behavior.

## Important limitations

**No live validation was performed.** Simultaneous central (scale) + peripheral
(debugger) operation depends on the specific BLE controller, Arduino core, and
ArduinoBLE implementation; compatibility is **unproven and may fail**. A Mac
debugger disconnect should not deliberately drop the scale, but no claim is
made about controller side effects. Hardware validation remains necessary.
The preexisting scale-timeout path calls `BLE.disconnect()`, which can also
drop the debugger connection; diagnostics do not introduce or change that path.
Existing scale-control behavior and known symptoms (including unreliable tare,
connection quirks, and learning/control limitations) are not fixed by this
diagnostics feature. No OTA/control/Wi-Fi/cloud capability is added.

Polling deliberately avoids an ArduinoBLE 2.1.0 notification hazard found in
source review: `src/utility/ATT.cpp` `ATT.handleNotify` iterates all connected
peers (including a scale), the CCCD is shared, and `src/utility/HCI.cpp`
`sendAclPkt` can poll-wait for TX credits. Therefore diagnostics must not add
periodic unsolicited notification traffic to the scale. In
`src/local/BLELocalCharacteristic.cpp`, local `writeValue` stores the value and
enters notification delivery only with Notify plus enabled CCCD; our
characteristics never have Notify, so these periodic refreshes remain local.
This is source review, **not live transport validation**. Normal ATT read
responses still have controller/backend latency and do not carry a hard
nonblocking guarantee. Peer slots are shared across roles; their configured
count does not prove controller dual-role support. These stack behaviors are
not fixed or simulated away by the diagnostics tests.
