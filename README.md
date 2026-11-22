# Moon Phase Clock

An ESP32-powered round-display clock that shows the current time, date, and daily moon phase — fetched from MET Norway and rendered with a 30-frame moon image cycle.

This project is an adaptation of a project from [nishad2m8](https://github.com/nishad2m8) & [LazydaysCr](https://github.com/LazydaysCr)

Modified with Claude.ai to add Moon Rise/Set time

## Features

- Live time & date via NTP.
- Daily moon phase name + matching moon image, pulled from MET Norway's Sunrise API
- Built with LVGL 8.3.11 UI designed in SquareLine Studio
- Moon rise and Moon set time displayed
- First-boot Wi-Fi and MET Norway contact setup through a local access point
- Automatic IP-based location and time-zone lookup
- Local network page for viewing or overriding location and time zone
- Nighttime auto-dimming of display colors, enabled by default
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

You can flash a prebuilt image with the [online web installer](https://zinfandel22.github.io/MoonPhaseClock/) instead of building with PlatformIO; see [Web installer](#web-installer).

1. **Open the project** in PlatformIO (VS Code extension).
2. **Wire the display** to your board per the table above. `SDA` and `SCL` on the display are used as SPI MOSI and SCK, not as I²C signals.
3. **Build and upload** using PlatformIO. Select `esp32dev` for NodeMCU-32S or `xiao_esp32c3` for XIAO ESP32-C3. A default build compiles both environments; select the correct one when uploading. On the command line, use `pio run -e esp32dev -t upload` or `pio run -e xiao_esp32c3 -t upload`.
4. **Open the Serial Monitor** at 115200 baud and reset the board. On first boot, the clock prints the setup access point name.
5. **Connect a phone or computer** to that open access point; no password is needed. If the setup page does not open automatically, browse to `http://192.168.4.1`.
6. **Enter the network and contact settings.** Leave the Wi-Fi password blank only for an open network; WPA passwords must be 8 to 63 characters. Enter an email address for MET Norway to contact you; it is sent in the API request's `User-Agent` header. The auto-dim checkbox is enabled by default.
7. **Save and connect.** The clock saves settings in ESP32 nonvolatile storage, restarts, and connects to Wi-Fi. Its assigned local IP address appears on the display for 10 seconds after connecting and is also printed in the Serial Monitor. It then syncs time via NTP and looks up its approximate location and time zone before fetching the daily moon phase.

The setup access point starts when no settings have been saved, when a saved contact email is missing or invalid, or when the saved Wi-Fi network cannot be reached. After upgrading from a version without a contact email, connect to the setup access point and enter the email and Wi-Fi password; the saved network name is retained. If connection fails, the form retains the saved network name and contact email; enter the Wi-Fi password again. Saved settings do not require editing `include/credentials.h`.

The setup access point is open and does not encrypt traffic. Provision the clock only in a trusted location, then disconnect from the setup network after configuration.

## Automatic location

On startup, the clock sends one HTTPS request to [ipwho.is](https://ipwhois.io/documentation) to estimate latitude, longitude, and time zone from its public IP address. The service receives that public IP. IP-based results are approximate and may be incorrect, especially with VPNs, mobile networks, or distant ISP gateways; they determine the moon times, image orientation, and local clock. The returned IANA time zone is converted using the bundled TzDbLookup database, including daylight-saving rules where available.

The clock caches the last successful automatic result for the current Wi-Fi network. If lookup fails, it uses that cache and retries after five minutes; rate-limit responses may delay the retry. If there is no cache, it displays **Location error** and waits for a successful lookup. Errors are also reported in the Serial Monitor. Manual locations from older firmware versions are not migrated. The service's free tier has usage limits, and changes to its certificates or time-zone data may require a firmware update.

## Network location and display settings

After connecting to Wi-Fi, the clock displays its assigned IP address for 10 seconds, including after a reconnection. From a device on the same network, open `http://<clock-ip>/` to view its current latitude, longitude, and IANA time zone. The address is also printed in the Serial Monitor; if it changes, use the new address.

To override automatic detection, select **Manual location and time zone**, enter decimal coordinates and a supported IANA name such as `Australia/Sydney` or `America/Los_Angeles`, and save. The clock restarts and retains the override until you select **Automatic IP-based location** and save again. Manual mode skips the IP lookup. A cached location from an older firmware may show an empty time-zone name until the next successful lookup.

In the **Display** section, check or uncheck auto-dimming and select **Save display setting**. This saves independently of the location settings and does not restart the clock. When enabled, the clock scales rendered RGB colors to approximately 50% from **10:00 p.m. until 8:00 a.m.** local time. It returns to full color outside those hours. Dimming starts only after the clock has a valid time and time zone; the always-on backlight itself is not controlled. Existing installations default to auto-dim enabled.

The network settings page uses unencrypted HTTP and has no password. Anyone on the local network can view or change these settings; use it only on a trusted network.

## Create a merged flash image

Run `pio run` to build both environments, or `pio run -e esp32dev` / `pio run -e xiao_esp32c3` to build one. When firmware is built, PlatformIO creates `.pio/build/<environment>/firmware-merged.bin`, containing the bootloader, partition table, boot data, and application, and copies it to `firmware/<environment>.bin` for the web installer. If the staged files are missing or need refreshing without a rebuild, run `pio run -t merge` for both, or `pio run -e <environment> -t merge` for one.

Flash a merged image at offset **`0x0`** using a flashing tool compatible with the selected board.

**Warning:** Flashing the merged image also overwrites the gaps between its components, including saved Wi-Fi and location settings. The clock will need to be configured again. Use the normal PlatformIO upload to retain those settings.

## Web installer

The [online web installer](https://zinfandel22.github.io/MoonPhaseClock/) uses ESP Web Tools to flash the staged images. To run the included `index.html` yourself, serve the project root over HTTPS or locally with `python -m http.server 8000` and open `http://localhost:8000/`. Opening the HTML file directly is not sufficient. An internet connection is needed to load ESP Web Tools.

Use a Web Serial-compatible browser such as Chrome or Edge, connect the board by USB, check the wiring shown on the page, and click **Connect**. The installer detects the chip family and selects the corresponding image; the board selector only highlights the wiring and does not choose the firmware. After flashing, follow the setup access point instructions above. Flashing through the installer overwrites saved settings, as described above.

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
