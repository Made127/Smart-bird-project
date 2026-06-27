# Battery module wiring

This project now supports a 1-cell lithium battery monitor for the 3.7V
2500mAh pack shown in the photo.

## Power wiring

| Battery/module pin | Connect to |
| --- | --- |
| B+ / battery red | Battery positive |
| B- / battery black | Battery negative / GND |
| OUT+ | Board power input if your board accepts the module output voltage |
| GND | ESP32 GND and all module GND |
| IN+ | Type-C/5V charging input positive, if this board exposes it |

Check the module output voltage with a multimeter before feeding the ESP32
board. Some charging boards output the raw battery voltage, while boost boards
may output 5V.

## Battery voltage sense

Do not connect battery B+ or OUT+ directly to an ESP32 ADC pin.

Default code configuration:

| Signal | ESP32-S3 |
| --- | --- |
| Battery sense midpoint | GPIO3 / ADC1_CH2 |
| Battery/module GND | GND |

Use a resistor divider:

```text
Battery B+ ---- 100K ---- GPIO3 ---- 100K ---- GND
```

With this divider, a full 4.2V battery becomes about 2.1V at GPIO1, which is
safe for the ESP32 ADC.

## Software behavior

- Serial log prints `Battery: voltage=...mV percent=...%`.
- App status shows battery percent and voltage.
- Lower than `BIRD_BATTERY_LOW_PERCENT` triggers the amber low-battery LED.
- If charger `CHG` / `FULL` pins are available later, set
  `BIRD_BATTERY_CHG_PIN` and `BIRD_BATTERY_FULL_PIN` in
  `components/Config/include/bird_test_config.h`.
