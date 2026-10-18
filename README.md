# AcaiaArduinoBLE
Acaia / Bookoo / Felicita Scale Gateway using the ArduinoBLE library for esp32-based devices.
This is an Arduino Library which can be found in the Arduino IDE Library Manager.

## Scale Compatibility

| Mfr    | Model   | Submodel | Firmware | Connection Performance | Auto-Tare | Auto-Start/Stop Timer | Auto-Reset Timer |
| ------ | ------- | ------- |------- |------ | ------ |------ |------ |
| Acaia  | Lunar   | USB-Micro <br>(Pre-2021) | v2.6.019 | Great | Yes | Yes | Untested
| Acaia  | Lunar   | USB-C <br> (2021 version)  | v1.0.016 | Hit or Miss    | Yes | Yes | Yes
| Acaia  | Pearl S | USB-Micro                  | v1.0.056 | Ok    | Yes | Yes | Yes
| Acaia  | Pearl S | USB-C                      | ----     | Ok    | Yes | Yes  | Yes
| Acaia  | Pyxis   | ----                       | v1.0.022 | Good  | Not Recommended (too sensitive) | Yes | Yes
| Bookoo | Themis  Mini | ----                       | v1.0.5   | Great | Yes | Yes | Yes 
| Bookoo | Themis Ultra  | ----                 | ----   | Great | Yes | Yes | Yes 
| Felicita | Arc   | ----                       | ----   | ---- | Yes | Yes | Yes 

Note on the Lunar USB-C (2021): upstream captures show that scale messages can be split across BLE notifications. Older versions of this library dropped weight messages that did not arrive in a single supported notification, which can stall weight-dependent shot timing. "New"-protocol scales are now parsed as a reassembled byte stream with checksum validation and resynchronization after invalid frames. Commands are spaced `CMD_SPACING_MS` apart as a precaution against back-to-back command loss. These are protective measures, not a confirmed explanation for every intermittent missing beep.

New-protocol connections time out if no valid weight arrives within `MAX_PACKET_PERIOD_MS` (5 seconds), including after initial connection. Other scale types retain their existing first-weight behavior. Pending valid weights are processed before deciding that a connection timed out.


## Requirements
This library is intended to be used with any arduino device which is compatible with the [ArduinoBLE](https://www.arduino.cc/reference/en/libraries/arduinoble/) library.

## Printed Circuit Board
![shotStopperV3 screenshot](https://github.com/user-attachments/assets/a09fe8fb-3705-44c0-88a2-07c61d67b8f6)

The included "shotStopper" example code uses the ShotStopper PCB to make it simple to control your espresso machine using the scale.

A kit can also be ordered by visiting [tatemazer.com](https://tatemazer.com/store)

If you choose to build your own from scratch, v2.0 is recommended as it requires only through-hole components

Join the discord for updates and support: https://discord.gg/NMXb5VYtre

[![Video showing developmnent of the shotStopper](https://img.youtube.com/vi/434hrQDGtxo/0.jpg)](https://youtu.be/434hrQDGtxo)

## Espresso Machine Compatibility

| Model | Powered by Machine (5V) | Brew State Detection Method | Officially Documented |
| ----- | ----------------------- | --------------------------- | ---------------- |
| GS3 | No, requires included power supply | Solenoid Valve (Reed Switch) | Yes |
| Linea Micra | Yes | Brew Switch | Yes | 
| Linea Mini* | Older, non-IoT machines may require a power supply | Brew Switch | Yes |
| Linea Mini R | Yes | Brew Switch | Yes |
| Silvia Pro (X) | Yes | Brew Button | Yes |
| Stone Espresso | Yes | Solenoid Valve (Reed Switch) | Yes |
| Ascaso Steel Duo PID | Untested | Brew Button | No
| Profitec Move | Yes | Brew Button | No |

*Ace Dotshot is compatible with the shotStopper. Also note, shot duration is automated at the scale with the shotStopper, making the dotShot redundant.

## ShotStopper Example Code Configuration

The following variables at the top of the shotStopper.ino file can be configured by the user:

`MOMENTARY`
* true for momentary switches such as GS3 AV, Rancilio Silvia Pro, etc.
* false for latching switches such as Linea Mini/Micra, stone, etc.

`REEDSWITCH`
* true if a reed switch on the brew solenoid is being used to determine the brew state. This is typically not necessary so set to FALSE by default. This feature is only available for non-momentary-switches.

`AUTOTARE`
* true by default. The scale will automatically tare when the shot is started, and, if MOMENTARY is false, will perform another tare when the switch latches to notify the user to return it to the home position.
* if set to false, the shotStopper will never send a tare command. It is the user's responsibility to tare before each shot. This may be helpful if the scale is not stable when the shot begins, and thus the scale is unable to tare reliably.

`TIMER_ONLY`
* false by default. disables brew-by-weight functionality and enables only automatic timer and tare

For latching paddles, takeover requires more than `MIN_SHOT_DURATION_S` (3 seconds) of elapsed time, the paddle still being on, and a valid weight received within `WEIGHT_FRESH_MS` (1 second). Stale data delays takeover and prints a warning once per shot; the paddle remains in manual control. If fresh weights resume while the paddle is still on and the maximum shot duration has not elapsed, takeover and its tare beep occur then. A beep is a tare command, not an acknowledgment that the scale executed it.

The `MAX_SHOT_DURATION_S` (50 seconds) limit uses elapsed time independently of weight updates. Machine-control output changes happen before Bluetooth timer-stop commands, and a disconnect releases a latched output before any reconnect attempt. Once latched, the paddle no longer stops the shot; keep an independent machine shutoff accessible during supervised testing.

If a latching shot ends by time or weight before takeover, the controller waits for a genuine debounced paddle release before accepting another start. Holding the paddle on, or receiving fresh weights later, cannot restart the timer, tare again, or trigger a late takeover. The machine remains under manual paddle control while waiting, and the ended shot stops collecting measurements. The temporary reed-switch ignore window does not count as a release.

## Validation

Run the host regression suite with `python3 tests/run.py`. It requires a C++17 compiler with AddressSanitizer and UndefinedBehaviorSanitizer support; it does not access Bluetooth or machine hardware. Set `CXX` to select a compiler.

For hardware verification, first record the original firmware's serial output during normal use until the intermittent failure occurs. Correlate weights, `Button Latched`, and `beep write successful` with the audible beeps; a successful write alone does not confirm command execution. Identify the actual controller board and preserve its existing firmware/recovery procedure before flashing a candidate build. Compare the same conditions on the controller, including cold connections, idle periods, consecutive starts, missing initial data, and disconnects after takeover, using supervised water-only runs and an independent way to stop the machine. Disconnects during active shots retain the existing restart-and-tare behavior when reconnecting with the paddle still on; include it in these observations. Record failures and final weight error against the baseline. Mac captures are useful parser fixtures, but cannot validate ArduinoBLE behavior or establish the original cause by themselves.

The prebuilt files in `examples/shotStopper/build` are not regenerated by source changes or host tests. Build firmware for the identified board to include these fixes.

These changes are incomplete reliability hardening, not a confirmed fix for every Lunar failure. Startup/command acceptance and ineffective tare followed by a false weight cutoff and post-takeover restart remain unresolved; see [the investigation](https://github.com/mrecachinas/acaiaarduinoble/issues/2). Actual-controller validation is still required before recommending deployment.

## Demo

You can find a demo on Youtube:

[![Video showing an shotStopper pulling a shot on a silvia pro](https://img.youtube.com/vi/oP3Cmke6daE/0.jpg)](https://www.youtube.com/shorts/oP3Cmke6daE)

## Scale Compatibility:

☑ Acaia Pyxis

☑ Acaia Lunar (usb-micro)

☑ Acaia Lunar 2021 (usb-c)

☑ Pearl S

☑ Felicita Arc

☑ Bookoo


## Bugs/Missing
1. Tare command is less reliable than pressing the tare button for pyxis
2. Only supports grams.

# Acknowledgement
This is largely a basic port of the  [LunarGateway](https://github.com/frowin/LunarGateway/) library written for the ESP32.

In addition to some minor notes from [pyacaia](https://github.com/lucapinello/pyacaia) library written for raspberryPI.

Felicita Arc support contributions from baettigp and A-TWJ

Bookoo contributions from philgood and same31

lunar 2019 contributions from jniebuhr
