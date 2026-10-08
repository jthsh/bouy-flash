# bouy-flash

Firmware for a battery powered model navigation buoy light. It runs a selectable flash pattern on an LED, remembers the chosen pattern between power cycles, and puts itself into a low power sleep when it is finished, when the battery runs low, or when you tell it to.

Written for the **ATtiny412** (via megaTinyCore), and also runs on an **ATmega328P** (Arduino Uno) for prototyping.

Current version: **1.3**

## Features

- Three built in flash patterns, easy to extend with your own
- Single button control: next pattern, previous pattern, sleep
- Pattern choice saved to EEPROM, so it survives power loss
- Automatic sleep after a set run time (12 hours by default)
- Power down sleep mode, woken by the button
- Battery voltage monitoring on the ATtiny412, with:
  - LED brightness compensation so output stays roughly constant as the battery drains
  - Low voltage cut off to protect lithium cells
- Profiles for 3 V primary cells (e.g. CR2032) and 3.7 V lithium cells
- Optional lightweight serial debug output

## Hardware

### ATtiny412 pinout

```
            +---U---+
  VDD    -> |1     8| <- GND
  PA6 TX <- |2     7| -> PA3 LED
  PA7 RX <- |3     6| -> PA0 UPDI
  PA1    <- |4     5| -> PA2 Button
            +-------+
```

| Function   | ATtiny412 | Arduino Uno |
|------------|-----------|-------------|
| Buoy LED   | PA3 (`LED_BUILTIN`) | `LED_BUILTIN` (pin 13) |
| Status LED | Same as buoy LED (PA1 is a good alternative) | Pin 9 (PWM) |
| Button     | PA2, to GND | Pin 2, to GND |
| Serial TX  | PA6 | Standard USB serial |

The button uses the internal pull up, so wire it between the pin and ground. If you move the button pin, the sleep and wake code needs updating to use the matching interrupt.

Unused pins on the ATtiny412 are set as pulled up inputs to reduce current draw.

If you are driving the LED through a transistor for more current and need an inverted output, uncomment the `pinConfigure` line in `setupPins()`.

Work out your LED dropper resistor at the battery's maximum voltage (`VCC_MAX_MV`).

## Building

### Requirements

- Arduino IDE
- [megaTinyCore](https://github.com/SpenceKonde/megaTinyCore) for the ATtiny412
- [OneButton](https://github.com/mathertel/OneButton) library (the sketch uses `OneButtonTiny`)
- A UPDI programmer for the ATtiny412

### ATtiny412 settings

| Setting | Value |
|---------|-------|
| Clock | 1 MHz |
| Startup time | 8 ms |
| millis | Enabled |
| BOD | Disabled |
| WDT | Disabled |
| Save EEPROM | Enabled (keeps the pattern setting across reflashes) |
| printf | Default |
| PWM | Default |

After changing fuse related settings (including the clock), run **Tools > Burn Bootloader**. There is no bootloader on the ATtiny412, but this is what writes the fuses. Easiest to just do it every time you change settings.

## Using it

1. **Power up.** With `SLEEP_AT_POWER_UP` enabled (the default) the device starts asleep. Press the button to wake it.
2. **Wake.** The LED sweeps from off to bright.
3. **Confirmation flash.** The LED flashes to show which pattern is selected: one flash for pattern A, two for B, and so on.
4. **Run.** The pattern repeats until the run time is reached or the button is pressed.

| Button action | Result |
|---------------|--------|
| Single press | Next pattern |
| Double press | Previous pattern |
| Long press (800 ms) | LED dims down and the device sleeps |

The device also goes to sleep automatically after `SHOW_MINS`, or when the battery drops below `MIN_VCC_MV`. Press the button to wake it again.

## Flash patterns

| # | Name | Description |
|---|------|-------------|
| A | Iso 4s | 2 s on, 2 s off |
| B | Fl(3) | 3 flashes of 300 ms, then a long pause |
| C | Multi flash | Rapid run of 300 ms flashes, then a long pause |

### Adding a pattern

Each pattern is a list of on/off steps with a duration in milliseconds:

```cpp
FlashStep patternD_steps[] = {
  {true, 500},  {false, 500},
  {true, 1500}, {false, 4500}
};

FlashPattern patternD = {
  .steps = patternD_steps,
  .numSteps = sizeof(patternD_steps) / sizeof(patternD_steps[0])
};
```

Then add it to the list:

```cpp
FlashPattern allPatterns[] = { patternA, patternB, patternC, patternD };
```

The pattern count and confirmation flashes update automatically. Flash space on the ATtiny412 is tight (4 KB), so disabling `SERIAL_OUT` frees around 190 bytes for more patterns.

## Configuration

Options at the top of `bouy-flash.ino`:

| Define | Default | Purpose |
|--------|---------|---------|
| `SERIAL_OUT` | On | Include serial debug output |
| `SLEEP_AT_POWER_UP` | On | Start asleep and wait for a button press |
| `SHOW_MINS` | `12*60` | Minutes to run before sleeping |
| `BATTERY_3V_PRIMARY` | On | Battery profile for 3 V primary cells |
| `BATTERY_3V7_LITHIUM` | Off | Battery profile for lithium cells |
| `DEBOUNCE_MS` | 50 | Button debounce time |
| `PRESS_MS` | 800 | Long press threshold |
| `CONFIRM_ON` / `CONFIRM_OFF` | 100 / 300 | Confirmation flash timing |

Enable only one battery profile.

### Battery profiles

| Profile | `VCC_MAX_MV` | `VCC_MIN_MV` | `MIN_VCC_MV` (cut off) |
|---------|--------------|--------------|------------------------|
| 3 V primary | 3000 | 2700 | 1820 |
| 3.7 V lithium | 4200 | 3300 | 3200 |

- **`VCC_MAX_MV`**: fresh or fully charged voltage. The LED is driven at reduced PWM here so it is not overly bright.
- **`VCC_MIN_MV`**: below this the LED is at full PWM, so brightness will start to fall.
- **`MIN_VCC_MV`**: the device sleeps at or below this voltage. For lithium cells, 3.2 V works well and 3.3 V gives more margin.

Brightness compensation uses an eight step PWM table per profile, tuned by eye. Adjust `pwm_table` if your LED or battery behaves differently. Voltage monitoring and compensation only run on the ATtiny412.

## Serial debug

With `SERIAL_OUT` enabled, output is sent at 19200 baud. On the ATtiny412 it is transmitted on PA6 using a small built in UART driver rather than the full `Serial` library, to save space. It prints a start up message and the supply voltage in millivolts on each pattern cycle.

## Changelog

| Version | Date | Notes |
|---------|------|-------|
| 1.3 | 2025-05-16 | Fixed fast I/O fallbacks so the 328P works again, option to disable serial, trimmed code to free space for patterns |
| 1.2 | 2025-05-14 | UART output, VCC reading, voltage based LED PWM, battery profiles |
| 1.1 | 2025-05-11 | Fixed ATtiny412 sleep and wake (was resetting), so `SLEEP_AT_POWER_UP` works |
| 1.0 | 2025-05-11 | Configurable debounce and long press times |
