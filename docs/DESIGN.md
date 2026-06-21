# System design

End-to-end reference for this pressure-controlled blower's electronics: hardware
bill of materials, pin map, wiring, firmware architecture, control algorithm,
and the non-obvious gotchas encountered while building it. (Originally built for
the "Graboid" costume, which is why the firmware identifies itself as `graboid`.)

If you only need to set up your environment and flash, see the
[README](../README.md) instead. This document explains *why* the firmware looks
the way it does.

---

## 1. Overview

A SparkFun Pro Micro ESP32-C3 reads air pressure from an LPS22 sensor and
drives a 12 V Delta blower through an N-MOSFET breakout, holding the inflated
costume at a configurable overpressure above ambient.

```
   USB 5V ──── or ────► VBUS ◄──── 5V ──── Buck ◄──── 12V supply ─┐
                          │                                        │
                     ┌────┴─────────────────┐                      │
                     │  SparkFun Pro Micro  │                      │
                     │       ESP32-C3       │                      │
                     │                      │                      │
              Qwiic ─┤ GPIO5/6  (I2C)       │       ┌────────────┐ │
                     │                      │ ─────►│ Adafruit   │ │
                     │ GPIO7   (PWM 1 kHz)  │       │ MOSFET     │◄┤
                     │ GPIO10  (status LED) │       │ PID 5648   │ │
                     │ GND ───── common GND │       │            │ │
                     └──────────────────────┘       └─────┬──────┘ │
                              │                            │ switched GND
                              │                            ▼       │
                     ┌────────┴────────┐              ┌────────┐   │
                     │   Adafruit      │              │ Delta  │◄──┘
                     │   LPS22         │◄── ambient ──│ BFB1012│
                     │   pressure      │              │ VH-AF00│
                     │   sensor        │              │ blower │
                     └─────────────────┘              └────────┘
```

Four subsystems:
1. **Sensing** — LPS22 on the Qwiic bus, polled at 10 Hz.
2. **Actuation** — fan switched via low-side N-MOSFET, PWM speed control.
3. **Control** — PI loop with a "boost to target, then hold" structure.
4. **Telemetry** — BLE GATT (Nordic UART Service), streams CSV to a phone
   running Bluefruit Connect for live plots and (in chunk 2) command input.

---

## 2. Hardware bill of materials

| Component                              | Vendor / P/N             | Role                                       |
|----------------------------------------|--------------------------|--------------------------------------------|
| SparkFun Pro Micro ESP32-C3            | SparkFun DEV-21925       | MCU                                        |
| Adafruit LPS22 STEMMA QT / Qwiic       | Adafruit PID 4633        | Barometric pressure sensor (I2C)           |
| Adafruit MOSFET Driver                 | Adafruit PID 5648        | Low-side N-MOSFET switch for the fan       |
| Delta BFB1012VH-AF00                   | Delta                    | 12 V DC blower (the fan)                   |
| 12 V DC supply ≥ 2 A                   | any                      | Powers the fan                             |
| Qwiic / STEMMA QT cable                | any                      | ESP32 ↔ LPS22                              |
| 12 V→5 V buck converter (optional)     | any 5 V output, ≥ 500 mA | Powers the ESP32-C3 off the same 12 V rail (untethered operation) |

Two viable power topologies:

- **Tethered (development).** USB-C powers the ESP32-C3; a separate 12 V
  supply only feeds the fan side. The two grounds **must be tied together**
  (more under wiring).
- **Untethered (costume use).** A buck converter drops the 12 V rail to 5 V
  and feeds the ESP32-C3's `VBUS` pin. The whole system runs from a single 12 V
  source. See [§4.3](#43-powering-the-esp32-c3-from-the-12-v-rail-optional).

---

## 3. Pin map

| GPIO     | Role            | Connected to                               | Notes                                       |
|----------|-----------------|--------------------------------------------|---------------------------------------------|
| GPIO 5   | I2C SDA         | Qwiic connector → LPS22 SDA                | SparkFun-specific Qwiic routing             |
| GPIO 6   | I2C SCL         | Qwiic connector → LPS22 SCL                | SparkFun-specific Qwiic routing             |
| GPIO 7   | Fan PWM output  | MOSFET breakout signal (`In`) pin          | 1 kHz PWM via LEDC                          |
| GPIO 10  | Status LED      | On-board STAT LED                          | Heartbeat: toggles per successful read      |
| GPIO 8,9 | (reserved)      | Strapping pins; avoid as outputs at boot   | The ESP32-C3 datasheet's "default" I2C; SparkFun chose 5/6 instead |
| GPIO 18,19| (reserved)     | USB D-/D+                                  | Don't touch                                 |
| GPIO 11–17| (reserved)     | SPI flash                                  | Don't touch                                 |

---

## 4. Wiring

### 4.1 I2C / LPS22 (Qwiic, plug-and-play)

Plug a Qwiic / STEMMA QT cable from the ESP32-C3's JST-SH connector into
either of the LPS22 board's two STEMMA QT connectors. Both LPS22 connectors
are wired to the same I2C bus; the second one is for daisy-chaining future
sensors. The breakout has 10 kΩ I2C pull-ups onboard — no externals required.

Default I2C address: **0x5D**.

### 4.2 Fan driver

```
12V supply (+) ────────────── MOSFET input: V+
12V supply (GND) ──────────── MOSFET input: GND
                              MOSFET input: GND ──── ESP32-C3 GND  ← common ground

Fan RED (V+) ──────────────── MOSFET output: "+"      (power passthrough)
Fan BLACK (GND) ───────────── MOSFET output: "–"/Out  (switched to ground)
Fan BLUE (FG tach) ────────── (not connected; reserved for future RPM telemetry)

ESP32-C3 GPIO 7 ────────────► MOSFET input: In  (signal)
```

The driver's input side is a STEMMA JST-PH connector and a matching 0.1" header
carrying `V+`, `GND`, and `In`. The output side is two screw terminals: `+`
(passes `V+` through) and `–`/`Out` (switched to ground when `In` is high). Since
all the fan current flows through the input `V+`/`GND`, prefer soldering the 12 V
supply to the 0.1" header pads rather than relying on the small JST connector.

**Critical**: the 12 V supply ground and the ESP32-C3 ground must be tied
together. Without a common ground reference, the MOSFET gate sees a floating
"low" and behavior is undefined.

**Optional safety pull-down**: a 10 kΩ resistor from GPIO 7 to GND keeps the
FET off while the ESP32-C3 boots (its GPIOs are inputs with no pulls for a
few ms during reset). Without it you may see the fan twitch on power-up.

### 4.3 Powering the ESP32-C3 from the 12 V rail (optional)

For an untethered costume — no USB cable to a laptop — drop the 12 V rail to
5 V with a buck converter and feed the ESP32-C3 on its VBUS pin. The
ESP32-C3 draws only 30–300 mA depending on Wi-Fi activity, so any cheap
**12 V→5 V buck rated ≥ 500 mA** is fine; a typical 5 A / 25 W module is
massively overkill but harmless (and runs cool because it's loafing).

```
12V supply (+) ─┬─ MOSFET breakout input V+
                └─ Buck IN+

12V supply (−) ─┬─ MOSFET breakout input GND ─── ESP32-C3 GND  (common ground)
                └─ Buck IN−

Buck OUT (+5V) ────── ESP32-C3 VBUS pin
Buck OUT (GND)  ───── ESP32-C3 GND
```

The common ground is already established through the MOSFET driver wiring,
so no new ground connection is required.

**Dual-supply caveat (dev workflow).** If you have the buck feeding 5 V into
VBUS *and* a USB-C cable plugged in for flashing/monitor, two 5 V sources
are present. The Pro Micro ESP32-C3 (DEV-21925) has a USB-VBUS protection
MOSFET that makes this safe on this board specifically, but for any other
board you'd want to verify before connecting both. Cleanest workflows:

- Add an SPDT switch or jumper on the buck output so you can isolate it
  during USB-tethered work.
- Or just unplug the 12 V supply while you're connected over USB.

**Inrush.** Hot-plugging the 12 V supply charges the buck's input capacitors
and starts the fan in the same instant. A 12 V supply with an overly
aggressive overcurrent trip can latch off during inrush; sizing the supply
at ≥ 2 A gives margin. A 1.5 A supply might just barely trip on cold start.

**Noise.** Cheap bucks are electrically noisy on their output. The ESP32-C3's
onboard 3.3 V LDO filters most of it before anything sensitive sees it,
which is fine for I2C and our digital signals. If LPS22 readings ever look
jittery on battery+buck vs USB power, add a **100 µF electrolytic + 0.1 µF
ceramic** at the VBUS pin and re-measure.

---

## 5. Firmware architecture

```
firmware/
├── CMakeLists.txt              top-level project file
├── sdkconfig.defaults          target, console, NimBLE config
└── main/
    ├── CMakeLists.txt          component registration; REQUIRES esp_driver_*, bt, nvs_flash
    ├── main.c                  I2C, LPS22 driver, PWM, control loop, telemetry
    ├── ble_nus.c               NimBLE host + GATT server (Nordic UART Service)
    └── ble_nus.h               public API: init, send, RX callback
```

(Environment setup and flashing live in the [README](../README.md); this
document is the design rationale.)

`main/main.c` contains:

1. **Pin and tuning constants** — all `#define`d at the top so tuning needs
   no code grep.
2. **LPS22 helpers** — `lps22_write_reg`, `lps22_read`, `lps22_read_pressure_hpa`.
   The LPS22HB requires bit 7 of the sub-address to be set for multi-byte
   auto-increment reads; on the newer LPS22HH it's don't-care. `lps22_read`
   sets it whenever `n > 1`, which works for either chip variant.
3. **Fan PWM helpers** — `fan_pwm_init`, `fan_set_duty(float 0–1)`.
4. **`app_main`** — initialises NVS (NimBLE needs it), peripherals, starts
   BLE advertising, samples ambient pressure with the fan off as the
   *baseline*, then runs the control loop forever — also emitting a CSV
   telemetry line over BLE each iteration.

`main/ble_nus.c` contains the NimBLE host task, GATT registration for the
NUS service, advertising start/restart on disconnect, and a thread-safe
`ble_nus_send()` that sends notifications on the TX characteristic.

ESP-IDF v6 split the monolithic `driver` component into per-peripheral
sub-components. `main/CMakeLists.txt` therefore declares:

```cmake
REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_ledc bt nvs_flash
```

Adding any new peripheral header means adding its `esp_driver_*` here too.
`bt` is the BLE stack (NimBLE), `nvs_flash` is required by NimBLE for
persistent state.

---

## 6. Control algorithm

### 6.1 Baseline

At boot, with the fan off, the firmware samples five LPS22 readings (over
~500 ms after a settle delay) and averages them into `baseline_hpa`. Target
pressure is then computed as:

```
target_hpa = baseline_hpa + TARGET_OVERPRESSURE_HPA      // default: +2.0 hPa
```

The baseline is **relative** because ambient atmospheric pressure varies
by 5–30 hPa with weather and altitude. Using a relative target means the
costume holds the same *inflation*, not the same absolute pressure, even
if the wearer drives to a different elevation or a weather front blows
through.

### 6.2 Boost-then-hold

```
error = target_hpa - p_hpa            // positive ⇒ need more pressure

if error > BOOST_BAND_HPA:            // far below target
    duty     = DUTY_MAX               // full speed
    integral = 0                      // don't accumulate during boost
else:
    integral += error * dt
    clamp integral to ±INTEGRAL_CLAMP
    duty = DUTY_FEEDFORWARD + KP*error + KI*integral

clamp duty to [DUTY_MIN, DUTY_MAX]
```

| Knob                        | Default | What it does                                          |
|-----------------------------|---------|--------------------------------------------------------|
| `TARGET_OVERPRESSURE_HPA`   | 2.0     | Inflation set-point above the baseline                 |
| `BOOST_BAND_HPA`            | 0.5     | While more than this below target → 100% duty          |
| `DUTY_FEEDFORWARD`          | 0.55    | Initial guess for the "hold" duty                      |
| `KP`                        | 0.50    | Proportional gain (duty per hPa)                       |
| `KI`                        | 0.20    | Integral gain (duty per hPa·s)                         |
| `INTEGRAL_CLAMP`            | 5.0     | Anti-windup limit on the integral                      |
| `DUTY_MIN`                  | 0.30    | Stall floor — BLDC fans don't spin reliably below this |
| `DUTY_MAX`                  | 1.00    | Ceiling                                                |
| `CONTROL_DT_MS`             | 100     | Loop period; matches LPS22 ODR of 10 Hz                 |

Why a `DUTY_MIN` floor: brushless DC fans need enough average voltage on the
windings for the commutation electronics to keep going. Below ~30 % duty the
fan can stall, especially against backpressure. Floor it.

Why a `DUTY_FEEDFORWARD`: with PI only, after reaching the band the duty
would have to climb up from `DUTY_MIN` via the integral, taking many seconds.
Starting from a reasonable hold-guess (55 %) makes the transition from
boost-phase to hold-phase smoother.

### 6.3 Failure mode

If an LPS22 read fails (I2C glitch, sensor dropped off the bus), the loop
logs a warning and *sets duty to 0*. The fan stops. This is the safe failure
direction — without pressure feedback, holding any duty risks unbounded
overpressure.

Sensor bring-up at boot is **non-fatal**: if the LPS22 doesn't answer (e.g.
it isn't plugged in yet), the firmware keeps BLE advertising, holds the fan at
0, lights the status LED solid, and re-probes once a second — it does *not*
crash-loop. The moment the sensor appears it samples the baseline and enters
the control loop. This lets the board be flashed and powered up before the
Qwiic sensor is wired.

### 6.4 Status LED

GPIO 10 toggles every successful control iteration. A **frozen LED means
the I2C bus or sensor died**, which is the same signal as fan-stop. Combined
they make for an at-a-glance health check.

---

## 7. PWM choice

The fan has no PWM input pin (it's a 3-wire, not a 4-wire fan), so speed is
controlled by chopping the supply rail with the MOSFET.

**Frequency: 1 kHz.** This is deliberately low.

The Adafruit PID 5648 breakout uses a **1N4007** flyback diode, which is a
50/60 Hz rectifier with ~5 µs reverse recovery. At 25 kHz PWM (the Intel
4-wire-fan standard) the diode wouldn't finish recovering before the next
on-cycle, causing brief shoot-through losses through the FET. 1 kHz is well
within the diode's comfort zone and only marginally audible — most of the
audible noise from a fan at any speed is aerodynamic, not switching whine.

If you swap to a driver with a fast Schottky flyback diode (e.g., SS54),
25 kHz becomes a viable choice and gets you fully inaudible operation.

---

## 7a. BLE telemetry

The ESP32-C3 advertises as `graboid-01` and exposes the **Nordic UART
Service (NUS)**:

- **Service UUID**: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- **RX characteristic** (write from phone): `6E400002-...` — used by chunk 2
  for command input.
- **TX characteristic** (notify to phone): `6E400003-...` — used for telemetry.

The control loop emits one CSV line per iteration on the TX characteristic:

```
<target_hpa>,<measured_hpa>,<duty_pct>,<temp_c>\n
e.g.   1015.21,1014.98,72.5,24.5
```

NUS is supported by every generic BLE client and is the protocol that
**Adafruit Bluefruit Connect** uses. In Bluefruit Connect's *Plotter* view,
the four CSV columns appear as four labelled lines updated in real time.
No app development required.

**Why NUS instead of a custom service:** widest tool support. Bluefruit
Connect, nRF Toolbox, LightBlue, and the various BLE terminal apps all
recognise it out of the box, so the same firmware works against whatever
phone-side tool you prefer.

**Connection params:** the ESP32-C3 requests a 247-byte ATT MTU at connect
(`CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=247`). iOS Core Bluetooth typically
negotiates 185–247, which fits the longest CSV line comfortably in one
notification. Look for `MTU negotiated = N` in the monitor log after each
phone connect to confirm.

**Failure handling:** `ble_nus_send()` is a no-op when no client is
connected, so the control loop is unaffected by the phone being absent.
On disconnect, `ble_nus.c` restarts advertising automatically; the phone
can reconnect later without a firmware reset.

---

## 8. Operating limits and physical reality

| Limit                                    | Value          | Source                                    |
|------------------------------------------|----------------|-------------------------------------------|
| Fan max static pressure (dead-headed)    | 4.86 hPa       | Delta BFB1012VH-AF00 datasheet            |
| Fan free-air flow                        | 38 CFM         | "                                         |
| Fan continuous current                   | 1.5 A typical, 1.8 A max | "                                |
| MOSFET breakout continuous rating         | 1.5 A          | Adafruit PID 5648 product page            |
| MOSFET breakout peak rating               | 3.0 A          | "                                         |

The **continuous current margin is tight**. During the boost phase at
100 % duty, the FET and JST connector on the breakout will run warm. Sustained
"can't reach target" operation is the worst case — the loop never leaves
boost. If you find the breakout getting hot, options:

- **Lower `DUTY_MAX`** to ~80–90 % to cap dissipation.
- **Reduce leakage** in the inflated costume so the fan reaches target faster.
- **Upgrade the driver** (e.g., IRLZ44N module, BTS3134, or a real H-bridge
  module) — drops in as a higher-current replacement on the same GPIO.

The **4.86 hPa pmax** is a hard ceiling. Aiming higher than that, even at 100 %
duty, simply will not happen — the fan will just stall against the
backpressure. The default `TARGET_OVERPRESSURE_HPA = 2.0` leaves headroom.

---

## 9. Gotchas encountered (and how they were fixed)

These all caught us during bring-up. They're documented so the next person
doesn't burn an afternoon on them.

| # | Symptom                                                                  | Cause                                                                                                          | Fix                                                                                                  |
|---|--------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------|
| 1 | `idf.py set-target esp32c3` errored: "build/ doesn't seem to be a CMake build directory"            | Stale `build/` from a previous partial attempt                                                                  | Delete `build/` manually; `idf.py` won't wipe a directory it doesn't recognise                       |
| 2 | "File project_description.json cannot be found" in the extension log     | The file is generated by the first build; doesn't exist until then                                              | Ignore. Build once and it appears.                                                                   |
| 3 | First build failed: `driver/gpio.h: No such file or directory`            | ESP-IDF v6 fully modularised drivers; the legacy `driver` meta-component no longer transitively exposes GPIO    | `REQUIRES esp_driver_gpio` (and analogous `esp_driver_*` for every other peripheral) in `main/CMakeLists.txt` |
| 4 | I2C scan on GPIO 8/9 returned nothing                                     | SparkFun routed the Qwiic connector on this board to GPIO 5/6, not the ESP32-C3's "default" 8/9                  | Use SDA=GPIO5, SCL=GPIO6                                                                             |
| 5 | Brute-force pin-pair scanner triggered a brownout when probing GPIO 20/21| Internal pull-ups on UART0 lines fought something on the rail briefly                                            | Don't probe those pins; once the right pair is known, lock it in                                     |
| 6 | LPS22 multi-byte reads returned only the first register repeated         | The LPS22HB requires bit 7 of the sub-address set for auto-increment                                            | `lps22_read` sets `reg | 0x80` whenever `n > 1`; safe on LPS22HH where the bit is don't-care        |
| 7 | (Future, if seen) Fan twitches during ESP32 boot                          | GPIO 7 floats for a few ms during reset; the MOSFET gate floats with it                                          | 10 kΩ pull-down from GPIO 7 to GND                                                                   |

---

## 10. Tuning guide

After flashing, the monitor logs one line per control iteration:

```
P=1015.05 hPa  err=+0.16  duty=68%  T=24.5C
```

`err` is `target - measured`. Positive ⇒ duty should be ramping up.

| Observed behaviour                                | Likely cause                       | Fix                                                       |
|---------------------------------------------------|------------------------------------|------------------------------------------------------------|
| Duty oscillates fast between high and low         | Loop gain too high                 | Halve `KP`, then `KI`                                      |
| Pressure sags below target and stays there        | Insufficient integral action       | Raise `KI`; check `DUTY_MAX = 1.0`                         |
| Pressure overshoots and stays above target        | Can't slow fan enough (DUTY_MIN floor) | Lower `DUTY_MIN` (risk stall) or lower `TARGET_OVERPRESSURE_HPA` |
| Fan won't spin at low duty                        | Below stall point                  | Raise `DUTY_MIN`                                            |
| Pressure never climbs even at 100 %               | Leakage > fan capacity, or wrong fan polarity | Tighten seals, or verify red→V+, black→switched GND |
| Status LED stops blinking                         | I2C or sensor failure              | Check Qwiic cable, check 0x5D on the bus                   |
| Breakout gets hot at sustained 100 % duty         | Current margin too thin            | Lower `DUTY_MAX` or upgrade the driver                     |

---

## 11. Future work

Reasonable next steps in roughly the order I'd pick them up:

- **Tach feedback.** Wire the fan's blue FG line to a GPIO via a 10 kΩ pull-up
  to 3.3 V and count edges with the PCNT peripheral. 4 pulses per revolution
  ⇒ RPM = pulses/sec × 15. Useful for diagnostics (lock detection: tach goes
  silent while duty > 0) and adaptive control (close inner loop on RPM,
  outer on pressure).
- **Pressure deadband / hysteresis.** Currently the controller tries to hit
  exactly `target_hpa`; a small deadband (e.g. ±0.1 hPa) would reduce
  needless duty wobble.
- **Boot self-test.** Spin the fan to 100 % for 1 s and confirm pressure
  *increases* by at least N hPa, otherwise refuse to run. Catches "fan
  unplugged" and "sensor and fan in different chambers" failures.
- ~~**Telemetry.** Currently logs over USB-Serial-JTAG. For an untethered
  costume, BLE GATT (esp_nimble) exposing current/target/duty makes tuning
  on a phone trivial.~~ ✅ Done. Outbound CSV streams via NUS to Bluefruit
  Connect's Plotter. **Chunk 2 (pending): command parser on the RX
  characteristic** — map Bluefruit Controller buttons to `boost`, `stop`,
  `target ±0.1 hPa`, `status`.
- **Filter on input pressure.** A small IIR (α=0.3 or so) smooths sensor
  noise without adding meaningful lag at 10 Hz.
- **Driver upgrade.** If the current margin keeps biting, swap to a
  higher-current MOSFET module. Same GPIO, same firmware. See
  [Appendix A](#appendix-a-mosfet-driver-upgrade-options).

---

## Appendix A: MOSFET driver upgrade options

The Adafruit MOSFET breakout (PID 5648) is **adequate for prototyping but
tight on current margin** (1.5 A continuous vs the fan's 1.5 A typical /
1.8 A max). If it runs hot, the JST connector browns under sustained boost,
or you simply want headroom, here are the upgrade paths in order of effort.

### Pre-built modules (easiest swap)

| Module                                                       | Current rating       | Flyback diode?           | Notes                                                                                                                  |
|--------------------------------------------------------------|----------------------|---------------------------|------------------------------------------------------------------------------------------------------------------------|
| Generic IRLZ44N / IRF520 / IRF3708 "MOSFET trigger module"   | 5–10 A typical       | **Usually no**            | Cheap (~$2–5 on Amazon/AliExpress). Same low-side topology, 3.3 V logic compatible. **Must add an external Schottky** across the fan (cathode to V+) — SS54 or 1N5822 — or you'll fry the FET on switch-off |
| Pololu Big MOSFET Slide Switch (any variant)                 | 5–15 A               | Yes (built-in protection) | More expensive but bulletproof, well-documented                                                                          |
| Adafruit DRV8871 DC Motor Driver (PID 3190)                  | 3.6 A continuous     | Yes (integrated body diodes) | It's an H-bridge: two inputs. For one-direction fan, tie one input low and PWM the other. **Slight firmware repinning** |

### Discrete on protoboard (most reliable, most work)

Build the same topology with discretes — preferred if you also want to raise
PWM back to 25 kHz for silent operation, since you choose the diode:

- **IRLZ44N** (TO-220 logic-level N-MOSFET, ~$1). Rds(on) ≈ 22 mΩ at
  Vgs=5 V, ≈ 50 mΩ at Vgs=3.3 V — plenty of headroom for 1.8 A.
- **SS54** or **1N5822** Schottky flyback diode across the fan terminals
  (cathode to V+, anode to FET drain). Fast recovery, suitable for 25 kHz PWM.
- **220 Ω** gate series resistor between GPIO 7 and the FET gate (limits
  inrush into the gate capacitance, slows edges slightly to reduce EMI).
- **10 kΩ** gate pull-down from gate to GND (keeps FET off while the
  ESP32-C3 boots and its GPIOs float).

### What stays the same

The firmware doesn't change at all for any of these — same GPIO 7, same
PWM frequency, same control loop. The only thing worth re-evaluating after
a discrete build with a fast Schottky is bumping `FAN_PWM_FREQ_HZ` from
1000 to 25000 in [main/main.c](../firmware/main/main.c) for inaudible operation.

### Recommendation

For least soldering with real current margin: **IRLZ44N module + an external
SS54 Schottky** soldered across the fan terminals. Cheap, reliable, generous
headroom, and the added diode lets you raise PWM to 25 kHz if you want it
silent.
