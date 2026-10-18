# Live Lunar firmware runner (macOS)

This development-only tool compiles the **actual** `shotStopper.ino` and
`AcaiaArduinoBLE.cpp` into a native library, then runs their setup, loop,
initialization, parser, command-spacing and reconnect logic against a real Lunar.
Only the Arduino platform and BLE transport are adapted. No ESP32 firmware is
flashed, and no espresso-machine GPIO is connected.

The runner supports the direct-input Micra configuration: `MOMENTARY=false`,
`REEDSWITCH=false`, `TIMER_ONLY=false`. It uses a real monotonic clock, virtual
GPIO, fresh in-memory EEPROM (the firmware initializes its defaults), and
CoreBluetooth through Bleak. The goal-weight BLE peripheral is virtual; the
companion phone app cannot connect to it.

This is **not** ESP32/ArduinoBLE-stack emulation. CPU timing, integer widths,
radio behavior, queues, power disturbances and physical switch signals differ.
Device-name metadata uses the advertisement name; advertisement-service and
appearance metadata are not reproduced. The adapter matches the library's
existing private characteristic-layout shim, with compile-time layout checks.
Older releases without that workaround, including `v3.3.0`, use their original
public BLE calls; the adapter does not add the newer write-without-response behavior.
It does not bypass `scale.init()` or force the firmware's connected/shot states.

## Setup

Use Python 3.12+ and an Apple Clang C++17 compiler:

```sh
python3.12 -m venv tools/lunar_sim/.venv
tools/lunar_sim/.venv/bin/python -m pip install -r tools/lunar_sim/requirements.txt
mkdir -p "$HOME/lunar-logs"
```

Run from **Terminal** and approve its macOS Bluetooth prompt. An application
without `NSBluetoothAlwaysUsageDescription` can be terminated by macOS before
showing a prompt; do not change the application's signing or bypass permissions.
Disconnect the Lunar from the ShotStopper and other applications before a trial.

```sh
tools/lunar_sim/.venv/bin/python tools/lunar_sim/runner.py --scan
```

Use the exact CoreBluetooth UUID printed for your Lunar, not a Bluetooth MAC.
Logs contain device information; keep them outside source control. The tool
refuses to overwrite an existing log.

## Monitor first

```sh
tools/lunar_sim/.venv/bin/python tools/lunar_sim/runner.py \
  --device YOUR_LUNAR_UUID --seconds 30 \
  --log "$HOME/lunar-logs/candidate-monitor.jsonl"
```

Monitor mode allows identification, subscriptions and heartbeats, but **blocks
tare and timer commands**. It records raw notifications, native firmware output,
GPIO/LED transitions, decoded-weight timestamps, and connection operations.

## Compare versions without switching branches

`--ref` exports only the three firmware source files from that commit into a
temporary build directory. Omitting it snapshots current working-tree firmware,
including uncommitted fixes. Each log includes the resolved commit and exact
source/adapter hashes. A published PR and local uncommitted fixes are different
candidates: use the PR's branch or commit explicitly when comparing them.

```sh
# Main, monitor only:
tools/lunar_sim/.venv/bin/python tools/lunar_sim/runner.py \
  --ref main --device YOUR_LUNAR_UUID --seconds 30 \
  --log "$HOME/lunar-logs/main-monitor.jsonl"

# Current working-tree candidate, six-second virtual-paddle trial:
tools/lunar_sim/.venv/bin/python tools/lunar_sim/runner.py \
  --device YOUR_LUNAR_UUID --scenario cycle --seconds 30 \
  --allow-scale-commands \
  --log "$HOME/lunar-logs/candidate-cycle.jsonl"
```

Repeat the second command with `--ref main` or the desired PR ref and a new log
name for an A/B comparison. No automatic download, checkout, commit or push occurs.
Use the same scale settings, load, and startup conditions. Software reconnection
is not equivalent to physically power-cycling the scale.

When matching a shipped controller, prefer its release commit over current
`main`. For example, `v3.3.0` is commit
`6c497bbcf752b3b0cb70bf6036013d4345a65c25`; later main commits still print the
same version string despite changed scanning and command-write behavior. Use
`--ref v3.3.0` if that tag is present locally, or the exact commit hash. The runner
does not fetch missing refs automatically.

`cycle` presses the virtual paddle once the **firmware** reports connected,
waits six seconds, releases it, and reports whether takeover occurred and the
virtual output stayed on. It does not wait for weight readiness outside the
firmware, so startup failures are observable. Observe the initial tare/timer
operations, the takeover tare near three seconds, and the final timer stop.
Several operations can beep, including timer start and cleanup stop; a fixed
total beep count is not a pass/fail criterion.

For an **established-stream** comparison, add `--start-after-weights`. This waits
for three distinct decoded updates, with no inter-update gap over one second,
before pressing the virtual paddle. The `warm_start_ready` event records that
condition. This deliberately changes the simulated operator's timing, not the
firmware: a successful warm start does not prove cold-start reliability. If
weights never arrive before `--seconds` expires, no paddle/tare/timer sequence is
started. Compare cold and warm trials separately.

Keep the scale's load unchanged during this latching-only cycle check. Reaching
the goal weight can legitimately stop the virtual output and fail that check;
use interactive mode to test weight-based completion.

The trial ends with an explicitly logged, best-effort timer stop if the runner
started the timer and has not sent a successful stop, then disconnects. That
cleanup is **outside the firmware**, not evidence of automatic weight-based
stopping. A successful GATT write does not prove that the scale executed it.
Cleanup uses a write with response when the characteristic supports it and keeps
the connection open for at least another 500 ms after STOP, including a STOP
already issued by the firmware. This avoids dropping a queued write by immediately
disconnecting. If the link is already down while a stop is still needed, cleanup
reports an error rather than claiming success; manually stop the timer if needed.
Notifications received after the native firmware stops remain in the log but are
marked `delivered=false`. Actual timer stopping still needs observation.

## Interactive debugging

Add `--interactive --allow-scale-commands` to monitor mode. Commands:

```text
status
paddle on
paddle off
goal 36
drop on
drop off
quit
```

Paddle and goal commands feed the platform interfaces, not private shot state.
After takeover, releasing the virtual paddle does not stop the shot, just as in
the firmware. Adding weight to the real scale can exercise weight-based stopping.
`quit` / Ctrl-C terminates the trial and performs cleanup.

`drop on` is **explicit fault injection**: raw notifications are still logged but
not delivered to the firmware. Its watchdog/reconnect behavior can then be
observed. `--fragment-size 3` deliberately splits delivered notifications and is
also logged as injection. Neither experiment proves that such loss/fragmentation
occurred naturally on the ESP32. All controls/fault injection require opt-in.
Runs are bounded by `--seconds` (default 120; maximum 3600).

## Offline checks

No Bluetooth or Bleak installation is needed for these tests:

```sh
python3.12 -m unittest discover -s tools/lunar_sim -p 'test_*.py'
```

They check command gating, cancellation cleanup, the real firmware's watchdog
and reconnect path, and historical main (`9c6c344`) versus the working tree under
identical **synthetic** fragmentation. The historical comparison is omitted when
that commit is absent from a shallow checkout, with a provenance test explicitly
marked skipped; the working-tree checks still run. For an offline demonstration:

```sh
python3.12 tools/lunar_sim/runner.py --fake --scenario cycle \
  --allow-scale-commands --seconds 20 \
  --log "$HOME/lunar-logs/fake-candidate.jsonl"
```

Exit status 0 means monitor mode observed decoded weights or the cycle's software
checks passed; 2 means those conditions were not met (including an aborted
cycle); 1 means a runtime/build error. None is a hardware safety certification.
