# Hardware and wiring

> ⚠️ Status: so far only the ESP32-S3 board has been tested (on USB). The steps below are the planned bring-up order.
> Do each step on its own and check it before moving to the next.

## Parts

| Part | Spec |
|---|---|
| Controller | ESP32-S3 DevKit, 44 pins, no PSRAM |
| Power | 12 V / 6 A supply, buck converter set to 5 V for the board |
| Pump | 12 V, ~3 A inrush current |
| Relay | Module, active LOW |
| Flyback diode | 1N4007 or 1N5819 |
| Capacitor | Electrolytic on the 12 V line |
| Water sensor | Analog |
| Button | Momentary, to GND |

## Pin map

| Signal | GPIO | Notes |
|---|---|---|
| Relay IN | 4 | Add a 10 kΩ pull-up from IN to 3.3 V so the relay stays OFF during boot |
| Button | 0 → 5 | BOOT button now; external button on GPIO5 with `INPUT_PULLUP` |
| Water sensor S | 1 | ADC1 (ADC2 is unusable while WiFi is on). **VCC to 3.3 V, not 5 V** |

## Bring-up order

1. **Hardware watchdog** (code only).
2. **External button** on GPIO5 and GND. Set `BTN_PIN 5`. Test start, stop and the 250 ms debounce.
3. **Relay without load.** VCC per module type, IN to GPIO4, common GND. Listen for the click, and check that it does *not* pulse during boot. A 5 V relay module driven by 3.3 V logic may not fully switch off. Check the module type first.
4. **Water sensor and dry-run protection.** Calibrate with the live value on the web page. Filter (EMA), configurable threshold stored in NVS, dry confirmed after 2 s, released after 60 s of water.
5. **Pump power stage.** Set the buck converter to 5 V and **measure before connecting**. Capacitor on the 12 V line (mind the polarity), diode reverse-biased across the pump. Check the event log for brownout resets when the pump starts.

## ⚠️ Rules

- **Never connect the pump without a flyback diode.**
- **Never feed the sensor 5 V.** ADC pins are 3.3 V only.
- Dry-run protection (step 4) must work **before** a real pump is connected.
- Changing the partition scheme **erases the event log** (LittleFS).
