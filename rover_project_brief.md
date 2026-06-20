# Brookstone Rover ESP32 Conversion — Claude Code Brief

## Project Summary
Converting a Brookstone Rover spy tank (tracked chassis with 2 DC motors)
into a BLE-gamepad-controlled robot using an ESP32, replacing the original
WiFi control board entirely.

## Hardware

| Component | Part | Notes |
|---|---|---|
| MCU | ESP32 DevKit V1 (38-pin, ESP-WROOM-32 module) | USB micro |
| Motor driver | DRV8833 dual H-bridge module | WWZMDiB breakout board |
| Power regulation | MP1584EN adjustable buck converter | Set to 5V output |
| Battery | 6x NiMH AA (7.2V nominal) | Rechargeable |
| Power switch | Toggle switch | On battery + line, before split |
| Chassis | Brookstone Rover tank treads | 2 independent tracks, left + right |

## Confirmed Pin Wiring (from physical board photo)

**Power:**
- Battery + → switch → splits to: MP1584 IN+ AND DRV8833 VM
- Battery − → common GND bus
- MP1584 OUT+ (5V) → ESP32 VIN
- MP1584 OUT− → common GND bus
- ESP32 3V3 → DRV8833 VCC (logic power)
- Common GND bus ties together: battery −, MP1584 GND, ESP32 GND, DRV8833 GND

**Motor control signals (ESP32 GPIO → DRV8833):**
- GPIO 14 → DRV8833 IN1 (left motor, channel A)
- GPIO 27 → DRV8833 IN2 (left motor, channel B)
- GPIO 26 → DRV8833 IN3 (right motor, channel A)
- GPIO 25 → DRV8833 IN4 (right motor, channel B)

**DRV8833 motor outputs:**
- OUT1/OUT2 → left motor terminals
- OUT3/OUT4 → right motor terminals
- (If a motor spins the wrong direction, swap its two OUT wires — no code change needed)

**GPIO pins confirmed AVOID on this board:**
- GPIO 12 (boot failure risk if HIGH at startup)
- GPIO 1/3 (TX0/RX0 — used by USB serial)

## Motor Control Logic (DRV8833)

| IN_a | IN_b | Result |
|---|---|---|
| PWM | LOW | Forward, speed = duty cycle |
| LOW | PWM | Reverse, speed = duty cycle |
| LOW | LOW | Coast (free spin) |
| HIGH | HIGH | Brake |

## Software Requirements

1. **Framework**: PlatformIO (preferred environment), Arduino framework for ESP32
2. **BLE gamepad support**: Use the **Bluepad32** library — supports PS4, PS5, Xbox, and generic BLE gamepads with robust pairing/reconnection handling
3. **Control scheme**:
   - Left analog stick Y-axis → left track speed/direction
   - Right analog stick Y-axis → right track speed/direction
   - (Tank-style differential drive control)
4. **PWM**: Use ESP32 `ledcWrite()` / LEDC peripheral for smooth speed control on all 4 GPIO pins (avoid `analogWrite` — not native on ESP32 in all cores)
5. **Safety features to include**:
   - Deadzone handling on analog stick input to prevent motor jitter at rest
   - Failsafe: if BLE controller disconnects, stop both motors immediately
   - Optional: gentle ramp-up/ramp-down rather than instant full power, to protect the gearbox


## What to ask Claude Code to do

Use the prompt below as your starting message to Claude Code.

---

### PROMPT FOR CLAUDE CODE

```
I'm building a tank-drive robot using an ESP32 DevKit V1 and a DRV8833 motor
driver, controlled by a BLE gamepad. Please set up a PlatformIO project for this.

Hardware:
- ESP32 DevKit V1 (38-pin, ESP-WROOM-32 module)
- DRV8833 dual H-bridge motor driver
- Two DC motors (left track, right track)
- Powered by 6x NiMH AA batteries (7.2V) through a buck converter to 5V for the ESP32

Pin wiring:
- GPIO 14 -> DRV8833 IN1 (left motor, channel A)
- GPIO 27 -> DRV8833 IN2 (left motor, channel B)
- GPIO 26 -> DRV8833 IN3 (right motor, channel A)
- GPIO 25 -> DRV8833 IN4 (right motor, channel B)

Requirements:
1. Use the Bluepad32 library for BLE gamepad support (PS4/PS5/Xbox/generic controllers)
2. Use ESP32 LEDC PWM (ledcWrite) for motor speed control, not analogWrite
3. Tank-style differential drive: left stick Y-axis controls left track,
   right stick Y-axis controls right track
4. Add a deadzone (~10%) on stick input to prevent motor jitter when centered
5. If the gamepad disconnects, immediately stop both motors (failsafe)
6. Add a basic ramp limiter so motor speed changes smoothly rather than
   snapping instantly to full power/reverse, to protect the gearbox
7. Set up platformio.ini with the correct board (esp32dev — this matches the
   ESP32 DevKit V1), framework (arduino), and add Bluepad32 as a lib dependency

Please create the full project structure, write the main.cpp with clear
comments, and explain how to flash it to the board over USB.
```

---

## Notes for You

- Before flashing, **double-check the MP1584 is set to 5V** with a multimeter — this is a one-time setup step, not part of the firmware
- The DRV8833 should already be wired per the diagrams we made earlier
- If a track runs backwards once you test it, you can fix it in software (swap which GPIO is "forward") instead of re-wiring, which is often easier
- Bluepad32 has its own PlatformIO library registry entry — Claude Code should be able to add it directly via `lib_deps`
