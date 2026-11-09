# Wiring

Target: ESP32 dev board (EL-SM-012, 30-pin), nRF24L01+PA+LNA (RX only), SSD1306 0.96" I2C OLED, 72x SK6812 RGBW, 3 buttons, latching power switch.

## Pin map

| Function            | ESP32 GPIO | Goes to                              |
|---------------------|-----------:|--------------------------------------|
| nRF24 SCK           | 18         | nRF SCK                              |
| nRF24 MISO          | 19         | nRF MISO                             |
| nRF24 MOSI          | 23         | nRF MOSI                             |
| nRF24 CSN           | 5          | nRF CSN                              |
| nRF24 CE            | 4          | nRF CE                               |
| nRF24 IRQ           | 27         | nRF IRQ                              |
| OLED SDA / SCL      | 21 / 22    | OLED SDA / SCL (3.3V)                |
| LED data            | 25         | Strip DIN                            |
| Button UP           | 32         | Button -> GND (internal pull-up)     |
| Button DOWN         | 33         | Button -> GND                        |
| Button MODE         | 13         | Button -> GND                        |

## Power

```
 5V battery pack (>=3A)
   |
 [latching power switch, >=3A, in the +5V line]
   |
   +--> SK6812 strip +5V / GND
   |        (also feed the far end of the strip with a second +5V/GND wire)
   +--> ESP32 VIN (5V)  -> onboard regulator -> 3V3
   |                                      |
   |                                      +--> OLED VCC
   |                                      +--> nRF24 VCC (see note)
 ALL GNDs common: battery, strip, ESP32, nRF24, OLED.
```

## LED data line

GPIO25 connects directly to the strip's DIN:

```
GPIO25 --> strip DIN
```

SK6812 at 5V nominally wants a logic-high of ~3.5V and the ESP32 gives 3.3V, so this is marginal. If the first pixels flicker or show wrong colors, a level shifter (e.g. 74AHCT125) is the usual fix. Keep the data wire short.

## nRF24L01+PA+LNA

- The PA+LNA module is power-hungry and noisy. Best: use the 3.3V adapter board that normally ships with it, fed from 5V.
- If wiring directly to the ESP32 3V3: put a 10-100uF capacitor plus a 0.1uF capacitor right across the module's VCC/GND pins.
- Module is 3.3V only. The IRQ pin is optional but saves SPI polling (code uses it).

## Buttons

Each button between its GPIO and GND. No external resistors needed.

## Power notes

- The latching switch cuts +5V to everything: strip, ESP32, radio and OLED. Switch only the +5V line; ground stays common.
- Current is capped in firmware by `LED_MAX_MA` (default 2650 mA) in `src/config.h`.
- SK6812s draw roughly 1 mA each even when black (~70 mA for 72). With the switch off this doesn't matter. For a "dark but still listening" state, switch the strip's +5V with a logic-level P-MOSFET and drive it from a spare GPIO.
