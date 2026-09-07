# Moon Phase Clock

An ESP32-powered round-display clock that shows the current time, date, and real-time moon phase — fetched from NASA's Dial-a-Moon API and rendered with a 30-frame moon image cycle.

This project is an adaptation of a project from [nishad2m8](https://github.com/nishad2m8) 
Modified with Claude.ai to add Moon Rise/Set time

## Features

- Live time & date via NTP.
- Current moon phase name + matching moon image, pulled from NASA's Dial-a-Moon API
- Built with LVGL 8.3.11 UI designed in SquareLine Studio
- Moon rise and Moon set time displayed

## Hardware

- ESP32 WROOM dev board
- GC9A01 240×240 round SPI display

## Wiring

| Display Pin | ESP32 GPIO |
|---|---|
| SDA | 17 |
| SCL | 16 |
| CS | 22 |
| DC | 21 |
| RST | 27 |
| BL | Not used (always on) |
| MISO | Not connected |

## Setup Instructions

1. **Open the project** in PlatformIO (VS Code extension).
2. **Add your WiFi credentials.** Open `include/credentials.h` and fill in:
   ```cpp
   #define WIFI_SSID "your-wifi-name"
   #define WIFI_PASSWORD "your-wifi-password"
   ```
3. **Update Lat/Long if not in Sydney** to your lat/long (use Google maps to find your lat/long)
4. **Update TimeZone if not in NSW/ACT** update src/main.cpp line 37 to match your timezone
5. **Add your email to src\main.cpp line 390** update "your-email@example.com" to your email address
6. **Wire the display** to the ESP32 per the table above.
7. **Build & upload** using PlatformIO (`esp32dev` environment is pre-configured for the GC9A01 driver).
8. **Power on.** The moon animation plays for 3 seconds on boot, then the clock connects to WiFi, syncs time via NTP, and fetches the current moon phase.

## How It Works

- Time and date update every second from the ESP32's system clock (kept accurate via periodic NTP sync).
- Moon phase data is fetched from NASA's Dial-a-Moon API:
  - Every **15 seconds** until the first successful fetch.
  - Every **1 hour** afterward, since moon age changes negligibly minute-to-minute.
- The moon's age (in days) is mapped to one of 30 image frames and an 8-phase name (New Moon, Waxing Crescent, First Quarter, Waxing Gibbous, Full Moon, Waning Gibbous, Last Quarter, Waning Crescent) using evenly-spaced day thresholds.

## Notes

- If colors look swapped, toggle `TFT_RGB_ORDER` in `platformio.ini`.
- If the display appears upside down, change `tft.setRotation(2)` in `main.cpp` to `0`.
