# Button and RGB LED wiring

## RGB LED module

The current configuration assumes a four-pin common-cathode RGB module:

| Module pin | ESP32-S3 |
| --- | --- |
| R | GPIO4 |
| G | GPIO5 |
| B | GPIO8 |
| GND / - | GND |

The configured LED polarity is active-high (`BIRD_RGB_LED_ACTIVE_HIGH` is `1`).

## Three-pin button module

| Module pin | ESP32-S3 |
| --- | --- |
| VCC | 3.3V |
| GND | GND |
| OUT | GPIO9 |

The button OUT signal is configured as low while released and high while
pressed. GPIO9 uses the ESP32 internal pull-down. Do not hold the button during
startup. The button is enabled only after the input has remained released for
500 ms, preventing startup glitches from entering sleep.

## Behavior

- Power-on and initialization: blue breathing.
- WiFi disconnected: blue breathing.
- WiFi connected: solid blue.
- Low battery: solid amber.
- Charging: breathing amber.
- Battery full: solid blue.
- Amber states have priority over all blue states.
- Short press: mute or unmute the speaker without changing the status LED.
- Long press for 2 seconds, then release: enter light sleep and turn the LED off.
- Press the button again: wake from light sleep and restart the application.
- Charging and low-battery LED APIs are ready, but require battery/charger
  detection pins before they can report real hardware state.

Light sleep is a software low-power state. True battery power-off and power-on
requires a latching power circuit; a three-pin signal button alone cannot cut
the battery supply.
