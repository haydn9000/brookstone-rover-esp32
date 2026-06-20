// =============================================================================
//  Brookstone Rover — ESP32 arcade-drive firmware
// -----------------------------------------------------------------------------
//  Hardware:
//    - ESP32 DevKit V1 (38-pin, ESP-WROOM-32)
//    - DRV8833 dual H-bridge motor driver
//    - Two DC motors (left track, right track)
//
//  Control:
//    - BLE gamepad via Bluepad32 (PS4 / PS5 / Xbox / generic)
//    - Arcade-style mixed drive:
//        * Left  stick Y-axis  -> throttle (forward / reverse)
//        * Right stick X-axis  -> steering (turn left / right)
//      Mixed to tracks: left = throttle + steer, right = throttle - steer.
//
//  Pin wiring (ESP32 GPIO -> DRV8833):
//    - GPIO 14 -> IN1 (left  motor, channel A)  [emits a brief PWM pulse at
//                  boot, so the left track may twitch once on power-up]
//    - GPIO 27 -> IN2 (left  motor, channel B)
//    - GPIO 26 -> IN3 (right motor, channel A)
//    - GPIO 25 -> IN4 (right motor, channel B)
//
//  DRV8833 in-in (PWM) control logic per motor (slow decay / drive-brake):
//    INa=HIGH, INb=PWM   -> forward (lower INb duty = faster)
//    INa=PWM,  INb=HIGH  -> reverse
//    INa=HIGH, INb=HIGH  -> brake
//    INa=LOW,  INb=LOW   -> coast (failsafe stop)
// =============================================================================

#include <Arduino.h>
#include <Bluepad32.h>

// -----------------------------------------------------------------------------
// Pin assignments
// -----------------------------------------------------------------------------
static const int PIN_LEFT_IN1  = 14;  // left  motor, channel A
static const int PIN_LEFT_IN2  = 27;  // left  motor, channel B
static const int PIN_RIGHT_IN3 = 26;  // right motor, channel A
static const int PIN_RIGHT_IN4 = 25;  // right motor, channel B

// -----------------------------------------------------------------------------
// LEDC (hardware PWM) configuration
//   Arduino-ESP32 core 2.x style API: ledcSetup() / ledcAttachPin() / ledcWrite()
//   One LEDC channel per H-bridge input (4 total).
// -----------------------------------------------------------------------------
static const int LEDC_CH_LEFT_IN1  = 0;
static const int LEDC_CH_LEFT_IN2  = 1;
static const int LEDC_CH_RIGHT_IN3 = 2;
static const int LEDC_CH_RIGHT_IN4 = 3;

static const int   PWM_FREQ_HZ   = 20000;  // 20 kHz: above audible range, smooth
static const int   PWM_RES_BITS  = 8;      // 8-bit resolution -> duty 0..255
static const int   PWM_MAX       = 255;    // max duty for PWM_RES_BITS

// -----------------------------------------------------------------------------
// Control tuning
// -----------------------------------------------------------------------------
// Bluepad32 analog axes report roughly -512..511. Up on the stick is negative,
// so we invert to make "stick up = forward".
static const int   AXIS_MAX      = 512;

// Deadzone (~10%) so a centered/idle stick produces zero output (no jitter).
static const int   DEADZONE      = (AXIS_MAX * 10) / 100;  // ~51

// Ramp limiter: maximum change in motor command per control update. Smaller =
// gentler acceleration/deceleration, which protects the gearbox.
static const int   RAMP_STEP     = 30;     // duty units per ~10 ms update

// Control loop period.
static const unsigned long CONTROL_PERIOD_MS = 10;

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------
ControllerPtr myControllers[BP32_MAX_GAMEPADS];

// Current (ramped) and target motor commands, range -PWM_MAX..PWM_MAX.
static int leftCurrent  = 0;
static int rightCurrent = 0;
static int leftTarget   = 0;
static int rightTarget  = 0;

static unsigned long lastControlMs = 0;

// -----------------------------------------------------------------------------
// Low-level motor driver
// -----------------------------------------------------------------------------
// Drive one motor. `speed` in -PWM_MAX..PWM_MAX (sign = direction, 0 = coast).
//
// Slow-decay ("drive/brake") PWM: hold one input HIGH and PWM the other with the
// INVERTED duty. Per the DRV8833 truth table this keeps the winding energized
// during the PWM-off interval, delivering more effective voltage/torque per duty
// than fast decay -> more top speed and pep. Trade-offs: motors run a little
// warmer and brake harder when the stick is released.
static void driveMotor(int chA, int chB, int speed) {
  speed = constrain(speed, -PWM_MAX, PWM_MAX);
  if (speed >= 0) {
    ledcWrite(chA, PWM_MAX);          // forward, slow decay
    ledcWrite(chB, PWM_MAX - speed);  // inverted duty on the other input
  } else {
    ledcWrite(chA, PWM_MAX - (-speed));
    ledcWrite(chB, PWM_MAX);          // reverse, slow decay
  }
}

// Immediately stop both motors (coast). Used by the failsafe.
static void stopMotors() {
  leftTarget = rightTarget = 0;
  leftCurrent = rightCurrent = 0;
  ledcWrite(LEDC_CH_LEFT_IN1, 0);
  ledcWrite(LEDC_CH_LEFT_IN2, 0);
  ledcWrite(LEDC_CH_RIGHT_IN3, 0);
  ledcWrite(LEDC_CH_RIGHT_IN4, 0);
}

// Convert a raw stick axis (-AXIS_MAX..AXIS_MAX, up = negative) into a motor
// command (-PWM_MAX..PWM_MAX, positive = forward) with deadzone handling.
static int axisToSpeed(int axis) {
  if (abs(axis) < DEADZONE) {
    return 0;
  }
  // Invert so pushing the stick up drives the track forward.
  int command = map(-axis, -AXIS_MAX, AXIS_MAX, -PWM_MAX, PWM_MAX);
  return constrain(command, -PWM_MAX, PWM_MAX);
}

// Convert a raw stick axis (-AXIS_MAX..AXIS_MAX) into a steering command
// (-PWM_MAX..PWM_MAX, positive = turn right) with deadzone handling. Unlike
// axisToSpeed() this is NOT inverted: pushing the stick right is positive.
static int axisToSteer(int axis) {
  if (abs(axis) < DEADZONE) {
    return 0;
  }
  int command = map(axis, -AXIS_MAX, AXIS_MAX, -PWM_MAX, PWM_MAX);
  return constrain(command, -PWM_MAX, PWM_MAX);
}

// Move `current` toward `target` by at most RAMP_STEP.
static int rampToward(int current, int target) {
  if (current < target) {
    return min(current + RAMP_STEP, target);
  } else if (current > target) {
    return max(current - RAMP_STEP, target);
  }
  return current;
}

// -----------------------------------------------------------------------------
// Bluepad32 connection callbacks
// -----------------------------------------------------------------------------
static void onConnectedController(ControllerPtr ctl) {
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (myControllers[i] == nullptr) {
      Serial.printf("Controller connected, index=%d\n", i);
      myControllers[i] = ctl;
      return;
    }
  }
  Serial.println("Controller connected, but no empty slot found");
}

static void onDisconnectedController(ControllerPtr ctl) {
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (myControllers[i] == ctl) {
      Serial.printf("Controller disconnected, index=%d\n", i);
      myControllers[i] = nullptr;
      break;
    }
  }
  // Failsafe: if no controllers remain connected, stop immediately.
  bool anyConnected = false;
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (myControllers[i] != nullptr) {
      anyConnected = true;
      break;
    }
  }
  if (!anyConnected) {
    stopMotors();
    Serial.println("No controllers connected -> motors stopped (failsafe)");
  }
}

// -----------------------------------------------------------------------------
// Read the first connected gamepad and update motor targets.
// -----------------------------------------------------------------------------
static void updateTargetsFromGamepad() {
  ControllerPtr gamepad = nullptr;
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (myControllers[i] && myControllers[i]->isConnected() &&
        myControllers[i]->isGamepad()) {
      gamepad = myControllers[i];
      break;
    }
  }

  if (gamepad == nullptr) {
    // No usable gamepad -> coast to a stop via the ramp limiter.
    leftTarget = 0;
    rightTarget = 0;
    return;
  }

  // Arcade mixing: left stick Y = throttle (forward/back), right stick X =
  // steering (turn left/right). Combine into per-track commands so one stick
  // drives and the other turns; pushing steer at zero throttle spins in place.
  int throttle = axisToSpeed(gamepad->axisY());   // left  stick Y, up = forward
  int steer    = axisToSteer(gamepad->axisRX());  // right stick X, right = right

  leftTarget  = constrain(throttle + steer, -PWM_MAX, PWM_MAX);
  rightTarget = constrain(throttle - steer, -PWM_MAX, PWM_MAX);
}

// -----------------------------------------------------------------------------
// Arduino entry points
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // Configure LEDC PWM channels and bind them to the H-bridge input pins.
  ledcSetup(LEDC_CH_LEFT_IN1,  PWM_FREQ_HZ, PWM_RES_BITS);
  ledcSetup(LEDC_CH_LEFT_IN2,  PWM_FREQ_HZ, PWM_RES_BITS);
  ledcSetup(LEDC_CH_RIGHT_IN3, PWM_FREQ_HZ, PWM_RES_BITS);
  ledcSetup(LEDC_CH_RIGHT_IN4, PWM_FREQ_HZ, PWM_RES_BITS);

  ledcAttachPin(PIN_LEFT_IN1,  LEDC_CH_LEFT_IN1);
  ledcAttachPin(PIN_LEFT_IN2,  LEDC_CH_LEFT_IN2);
  ledcAttachPin(PIN_RIGHT_IN3, LEDC_CH_RIGHT_IN3);
  ledcAttachPin(PIN_RIGHT_IN4, LEDC_CH_RIGHT_IN4);

  stopMotors();  // start safely stopped

  // Initialize Bluepad32.
  const uint8_t* addr = BP32.localBdAddress();
  Serial.printf("Bluepad32 firmware: %s\n", BP32.firmwareVersion());
  Serial.printf("BD Addr: %02X:%02X:%02X:%02X:%02X:%02X\n",
                addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

  BP32.setup(&onConnectedController, &onDisconnectedController);

  // Mice / keyboards are not used; keep only gamepads.
  BP32.enableVirtualDevice(false);

  // Uncomment once to clear stored pairings if a controller won't reconnect:
  // BP32.forgetBluetoothKeys();
}

void loop() {
  // Poll Bluepad32. Returns true when fresh controller data is available.
  bool dataUpdated = BP32.update();
  if (dataUpdated) {
    updateTargetsFromGamepad();
  }

  // Run the ramp limiter + motor output on a fixed cadence.
  unsigned long now = millis();
  if (now - lastControlMs >= CONTROL_PERIOD_MS) {
    lastControlMs = now;

    leftCurrent  = rampToward(leftCurrent,  leftTarget);
    rightCurrent = rampToward(rightCurrent, rightTarget);

    driveMotor(LEDC_CH_LEFT_IN1,  LEDC_CH_LEFT_IN2,  leftCurrent);
    driveMotor(LEDC_CH_RIGHT_IN3, LEDC_CH_RIGHT_IN4, rightCurrent);
  }

  // Throttled status print to diagnose connection / drive problems.
  static unsigned long lastDebugMs = 0;
  if (now - lastDebugMs >= 500) {
    lastDebugMs = now;
    ControllerPtr gp = nullptr;
    for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
      if (myControllers[i] && myControllers[i]->isConnected()) {
        gp = myControllers[i];
        break;
      }
    }
    if (gp == nullptr) {
      Serial.println("[status] no controller connected (scanning for pairing)...");
    } else {
      Serial.printf(
        "[status] axisX=%d axisY=%d axisRX=%d axisRY=%d brake=%d throttle=%d buttons=%d dpad=%d  L=%d R=%d\n",
        gp->axisX(), gp->axisY(), gp->axisRX(), gp->axisRY(),
        gp->brake(), gp->throttle(), gp->buttons(), gp->dpad(),
        leftCurrent, rightCurrent
      );
    }
  }

  // Yield to the Bluetooth stack.
  delay(1);
}
