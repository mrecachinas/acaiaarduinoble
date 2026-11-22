# Bench testing on real hardware

These tools test ShotStopper firmware on the actual controller and scale **without the espresso machine running**. They reproduce the November 2026 bench tests recorded in [issue #2](https://github.com/mrecachinas/acaiaarduinoble/issues/2).

The machine's paddle input isn't readable when the machine is unpowered, so shot tests use a **bench build**: firmware compiled with `-DSHOTSTOPPER_BENCH=1`, which replaces the paddle with serial commands, can suppress tares to inject faults, and never writes EEPROM.

> **Never leave a bench build installed in a machine.** It ignores the real paddle. Always flash the normal build back before reassembling. The boot log of a bench build starts with `BENCH TEST BUILD`.

Requirements: macOS or Linux, Python 3.9+ (standard library only), `arduino-cli`, and a USB-C **data** cable.

## 1. Safe setup

1. Turn the machine off and **unplug it from the wall**.
2. Open the machine and photograph the ShotStopper's connections.
3. Disconnect the ShotStopper's **red/black machine-power cable**, as Tate Mazer recommends before powering the board over USB. The paddle harness can stay connected.
4. Connect the ShotStopper to your computer, find its port with `ls /dev/cu.usbmodem*` (macOS), and set it: `PORT=/dev/cu.usbmodemXXXX`.

Keep the machine unplugged for every test below.

## 2. Back up, build and flash

Back up the whole flash once, before writing anything. It includes the bootloader and your saved target weight and offset:

```sh
ESPTOOL=$(ls -d ~/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool | tail -1)  # ~/.arduino15 on Linux
"$ESPTOOL" --port "$PORT" chip-id            # ESP32-C3 or ESP32-S3
"$ESPTOOL" --port "$PORT" read-flash 0 ALL ~/shotstopper-backup.bin
```

Build with **ESP32 Arduino core 3.3.3** and ArduinoBLE 2.1.0; a core 3.3.12 build crashed during Bluetooth startup on an ESP32-C3 ShotStopper. Use `esp32s3` instead of `esp32c3` for S3 boards.

```sh
arduino-cli core install esp32:esp32@3.3.3
arduino-cli lib install ArduinoBLE@2.1.0
BOARD=esp32:esp32:esp32c3:CDCOnBoot=cdc

# Bench build
arduino-cli compile -b $BOARD --library . \
  --build-property "compiler.cpp.extra_flags=-DSHOTSTOPPER_BENCH=1" \
  --output-dir build/bench examples/shotStopper

# Normal build, to flash back afterwards
arduino-cli compile -b $BOARD --library . --output-dir build/normal examples/shotStopper
```

Flash only the application. This keeps the bootloader, partition table and saved settings, provided the board uses the default 4 MB partition layout (true for the v3.3.0 release):

```sh
"$ESPTOOL" --port "$PORT" --after no-reset write-flash 0x10000 build/bench/shotStopper.ino.bin
"$ESPTOOL" --port "$PORT" --after no-reset verify-flash 0x10000 build/bench/shotStopper.ino.bin
```

To restore the original firmware completely: `"$ESPTOOL" --port "$PORT" write-flash 0 ~/shotstopper-backup.bin`.

## 3. Tools

All tools refuse to overwrite an existing log. Logs contain device addresses, so keep them out of Git.

| Tool | Use |
|---|---|
| `serial_log.py --port P --log F [--seconds S] [--reset]` | Timestamped serial capture. `--reset` reboots the board first. Works with any build. |
| `reconnect_trials.py --port P --log F [--trials N]` | Resets the board N times and reports how long each takes to stream weights. Timing starts at the firmware's ready banner, so output from before a reset isn't counted. Works with any build. |
| `bench_driver.py --port P --log F [--script "..."]` | Bench builds only. Sends paddle and tare-fault commands and logs the result. The console shows events and about one weight per second; the log has everything. |

Bench-driver commands, typed one per line or given comma-separated with `--script`: `1`/`0` paddle on/off, `n` normal tares, `a` skip the shot-start tare, `b` skip both the shot-start and takeover tares, `wait S`, `quit`.

## 4. Procedures

Run these from `tools/bench`. Turn the scale on, with no phone app connected to it, and put a cup on it. Unless noted, pour slowly from t=10 s at about 2 g/s, roughly an espresso's pace after a Micra pre-brew.

**Stop point:** a shot stops by weight at `goal − offset`. In a bench build the learned offset still changes in memory after each run, though nothing is saved, so check the latest `I detected a final weight … offset` line or reset the board to reload the saved value.

| # | Test | How | Pass criteria |
|---|---|---|---|
| 1 | Boot and stream | `serial_log.py --reset --seconds 60`, scale on | Boots without `Guru Meditation`; prints the saved goal; LED leaves red; about 10 weights per second with no gaps over 1 s. |
| 2 | Startup reliability | `reconnect_trials.py --trials 10` | Every trial streams weights within 60 s. Record silent connections (`no weight since connect`) and discovery failures; they should recover on their own. |
| 3 | Scale power cycle | Log while you turn the scale off, then on | `timeout!` about 5 s after the last weight, then automatic reconnection and weights. |
| 4 | Normal shot | `bench_driver.py --script "n,1,wait 6,0"`, pour from t=10 | One `shot started`; the scale tares to 0; `Button Latched` at about 3.0 s; no stop during the pause; `weight achieved` near the stop point; `stop timer` and a resend about 1 s later. |
| 5 | Held paddle | Script `"n,1"`, pour quickly after the second beep; later type `0`, `wait 1`, `1` | After the weight stop: `waiting for paddle release before next shot` and no new shot while held. After release and press: a normal new shot. |
| 6 | 50 s limit | Script `"n,1,wait 65,0"`, no pour | `ShotEnded by time` at about 50 s, output low, no restart while held. |
| 7 | Scale loss mid-shot | Script `"n,1"`; after the second beep turn the scale off, wait 10 s, turn it on | `timeout!` about 5 s after the last weight, then `wrote low` and `ShotEnded by disconnect`. After reconnecting with the paddle still on: no new shot until `0` then `1`. |
| 8 | Failed shot-start tare | Cup above the stop point; script `"a,1,wait 6,0"`, pour from t=10 | `initial tare looks ineffective` at takeover, `tare confirmed` within about 0.5 s, then a normal weight stop. |
| 9 | Both tares failed | Cup above the stop point; script `"b,1"`, no pour; afterwards `0`, `n` | `tare not confirmed` 1.5 s after takeover, `ShotEnded by tare fault`, output low, no restart while held, no `I detected a final weight`. |

Re-run test 1 after flashing the normal build back: the boot log must not contain `BENCH`.

## 5. What bench tests can't show

They don't exercise the real paddle input, the optocoupler, the pump, pre-brew timing or machine power. After reassembly, confirm with supervised water-only shots and an independent way to stop the machine.
