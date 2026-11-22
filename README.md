# Moon Phase Clock

An ESP32-powered round-display clock that shows the current time, date, and daily moon phase — fetched from MET Norway and rendered with a 30-frame moon image cycle.

This project is an adaptation of a project from [nishad2m8](https://github.com/nishad2m8) & [LazydaysCr](https://github.com/LazydaysCr)

Modified with Claude.ai to add Moon Rise/Set time

## Features

- Live time & date via NTP.
- Daily moon phase name + matching moon image, pulled from MET Norway's Sunrise API
- Built with LVGL 8.3.11 UI designed in SquareLine Studio
- Moon rise and Moon set time displayed
- First-boot Wi-Fi, location, time-zone, and MET Norway contact setup through a local access point
- Moon images automatically rotate 180° for locations south of the equator

## Hardware

- ESP32S NodeMCU (NodeMCU-32S) or Seeed Studio XIAO ESP32-C3
- GC9A01 240×240 round SPI display

## Wiring

| GC9A01 Pin | NodeMCU-32S GPIO | XIAO ESP32-C3 pin (GPIO) |
|---|---|---|
| SDA / MOSI | 23 | D4 (6) |
| SCL / SCK | 18 | D5 (7) |
| CS | 22 | D2 (4) |
| DC | 21 | D3 (5) |
| RST | 27 | D1 (3) |
| BL | Not connected (always on) | Not connected (always on) |
| MISO | Not connected | Not connected |

## Setup Instructions

1. **Open the project** in PlatformIO (VS Code extension).
2. **Wire the display** to your board per the table above. `SDA` and `SCL` on the display are used as SPI MOSI and SCK, not as I²C signals.
3. **Build and upload** using PlatformIO. Select `esp32dev` for NodeMCU-32S or `xiao_esp32c3` for XIAO ESP32-C3. The default is `esp32dev`. On the command line, use `pio run -e esp32dev -t upload` or `pio run -e xiao_esp32c3 -t upload`.
4. **Open the Serial Monitor** at 115200 baud and reset the board. On first boot, the clock prints the setup access point name.
5. **Connect a phone or computer** to that open access point; no password is needed. If the setup page does not open automatically, browse to `http://192.168.4.1`.
6. **Enter the network, location, and contact settings.** Use decimal latitude and longitude; a negative latitude automatically rotates the moon images 180° for the Southern Hemisphere. The time-zone field takes a POSIX TZ rule, for example `PST8PDT,M3.2.0,M11.1.0`. Leave the Wi-Fi password blank only for an open network; WPA passwords must be 8 to 63 characters. Enter an email address for MET Norway to contact you; it is sent in the API request's `User-Agent` header.
7. **Save and connect.** The clock saves settings in ESP32 nonvolatile storage, restarts, connects to Wi-Fi, syncs time via NTP, and fetches the daily moon phase.

The setup access point starts when no settings have been saved, when a saved contact email is missing or invalid, or when the saved Wi-Fi network cannot be reached. After upgrading from a version without a contact email, connect to the setup access point and enter the email and Wi-Fi password; the saved network name, location, and time zone are retained. If connection fails, the form retains the saved location, time zone, and contact email; enter the Wi-Fi password again. Saved settings do not require editing `include/credentials.h`.

The setup access point is open and does not encrypt traffic. Provision the clock only in a trusted location, then disconnect from the setup network after configuration.

## Create a merged flash image

The merged image is created automatically as part of a standard PlatformIO build. From the project directory, run `pio run -e esp32dev` or `pio run -e xiao_esp32c3`; PlatformIO creates `.pio/build/<environment>/firmware-merged.bin`, containing the bootloader, partition table, boot data, and application. Flash this file at offset **`0x0`** using a flashing tool compatible with the selected board. You can also run `pio run -e <environment> -t merge` to build the merged image explicitly.

**Warning:** Flashing the merged image also overwrites the gaps between its components, including the saved Wi-Fi and location settings. The clock will need to be configured again. Use the normal PlatformIO upload to retain those settings.

## How It Works

- Time and date update every second from the ESP32's system clock (kept accurate via periodic NTP sync).
- Moon phase and rise/set data are fetched together from MET Norway's Sunrise API:
  - Every **15 seconds** until the first successful fetch.
  - Every **1 hour** afterward. MET Norway reports a phase for the day, so the display does not track hourly changes in phase.
- The reported phase angle is mapped to one of 30 image frames and an 8-phase name (New Moon, Waxing Crescent, First Quarter, Waxing Gibbous, Full Moon, Waning Gibbous, Last Quarter, Waning Crescent).
- If moon data cannot be retrieved or the phase is invalid, the phase label shows **API Fail**. The last moon image remains displayed, and a later successful fetch restores the phase name. The retry schedule above is unchanged.

## Notes

- If colors look swapped, toggle `TFT_RGB_ORDER` in `platformio.ini`.
- If the display appears upside down, adjust the value passed to `tft.setRotation()` in `src/main.cpp`.
