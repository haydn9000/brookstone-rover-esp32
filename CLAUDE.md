# CLAUDE.md

Guidance for AI coding agents (Claude Code, Copilot, etc.) working in this
repository.

## What this project is

Firmware for a Brookstone Rover tank chassis converted to **ESP32 + DRV8833 +
BLE gamepad** control. Single-sketch Arduino/PlatformIO project. The entire
application lives in [src/main.cpp](src/main.cpp).

## Repository layout

- `src/main.cpp`: the entire firmware (see Architecture below).
- `platformio.ini`: build config and the Bluepad32 core override.
- `README.md`: user-facing setup, wiring, and usage docs.
- `img/`: rendered images used by the README (`.png`, `.gif`, `.jpg`).
- `hardware/`: editable hardware design sources (Fritzing `wiring-diagram.fzz`).

## Build, flash, monitor

PlatformIO project. Environment: `esp32doit-devkit-v1`.

```sh
pio run                      # build
pio run --target upload      # flash over USB (board on a COM port, e.g. COM11)
pio device monitor           # serial @ 115200
```

- **Windows / PowerShell:** the CLI is often not on `PATH`. Prefix every
  command with:
  `$env:PATH += ";$HOME\.platformio\penv\Scripts";`
- Always confirm a change still compiles with `pio run -e esp32doit-devkit-v1`
  before declaring it done.
- Close the serial monitor before uploading. It holds the COM port and the
  upload will fail with a port-busy error.

## Toolchain specifics (do not "modernize" blindly)

- **Arduino core 2.0.17** (bundled by the Bluepad32 package), *not* core 3.x.
  Use the **2.x LEDC API**: `ledcSetup()` / `ledcAttachPin()` /
  `ledcWrite(channel, duty)`. Do **not** switch to the 3.x
  `ledcAttach(pin, freq, res)` / `ledcWrite(pin, duty)` API. It will not
  compile here.
- Bluepad32 is supplied via `platform_packages` in
  [platformio.ini](platformio.ini) (a precompiled core with BTstack). There is
  **no `lib_deps` entry** for it and none is needed. Do not add the stock ESP32
  core, it cannot pair Classic/BLE gamepads.

## Architecture (single file)

`src/main.cpp`, in order:

1. **Pin + PWM config**: `PIN_LEFT_IN1/IN2`, `PIN_RIGHT_IN3/IN4`; four LEDC
   channels (0-3) at 20 kHz / 8-bit (`PWM_MAX = 255`).
2. **Tuning constants**: `AXIS_MAX`, `DEADZONE`, `RAMP_STEP`,
   `CONTROL_PERIOD_MS`.
3. **`driveMotor(chA, chB, speed)`**: **slow-decay** PWM, one input held HIGH,
   the other gets the inverted duty (`PWM_MAX - speed`). Sign of `speed` sets
   direction. Keep this slow-decay scheme unless explicitly asked to change it.
4. **`stopMotors()`**: failsafe, zero all targets/currents and write 0 to all
   channels (coast).
5. **`axisToSpeed()` / `axisToSteer()`**: deadzone + map to +/-`PWM_MAX`.
   `axisToSpeed` inverts (stick up = forward); `axisToSteer` does not.
6. **`rampToward()`**: limits command change per tick to `RAMP_STEP`.
7. **Bluepad32 callbacks**: `onConnectedController` /
   `onDisconnectedController` (the latter stops motors when none remain).
8. **`updateTargetsFromGamepad()`**: **arcade mixing**,
   `throttle = axisToSpeed(axisY())`, `steer = axisToSteer(axisRX())`,
   `left = throttle + steer`, `right = throttle - steer`.
9. **`setup()` / `loop()`**: LEDC init, BP32 setup; loop polls BP32, runs the
   ramp + motor output on a 10 ms cadence, and prints a throttled `[status]`
   line every 500 ms.

## Control scheme

Arcade drive: **left stick Y = throttle**, **right stick X = steering**. If the
user asks to go back to tank drive, map `axisY()` to left and `axisRY()` to
right directly with no mixing.

## Hardware facts that affect the code (verified)

- **DRV8833 `EEP` / `nSLEEP` must be HIGH** (wired to ESP32 `3V3`) or the chip
  stays asleep and no motion happens regardless of firmware. Never drive `EEP`
  from the >6.5 V motor rail.
- This DRV8833 breakout is **single-supply** (VCC = motor supply; no separate
  VM pin). Current-sense pins are tied to GND on-board (no current limiting).
- **GPIO 14 emits a PWM pulse at boot** -> the left track twitches once on
  power-up. Harmless. To fix, remap `PIN_LEFT_IN1` to a boot-clean pin
  (e.g. GPIO 13). This requires the user to physically move the wire, so
  always confirm before changing the pin.
- Avoid GPIO 12 (boot strapping) and GPIO 1/3 (USB serial).

## Conventions

- Keep everything in `src/main.cpp` unless the user asks to split it.
- Keep the header comment block in `main.cpp` (and the README) in sync with
  actual behavior when you change drive mode, control scheme, or pins.
- Tuning is done via the named constants near the top. Prefer changing those
  over sprinkling magic numbers.
- Do not add libraries unless required; this project deliberately has no
  `lib_deps`.

## Gotchas checklist before saying "done"

- [ ] Compiles with `pio run -e esp32doit-devkit-v1`.
- [ ] LEDC calls use the **2.x** API.
- [ ] Header comment in `main.cpp` still matches behavior.
- [ ] No new `lib_deps` / stock-core additions.
- [ ] Pin changes that need rewiring were confirmed with the user.
