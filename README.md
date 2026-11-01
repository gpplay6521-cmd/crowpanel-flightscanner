# CrowPanel FlightScanner

A live ADS-B flight radar for the [Elecrow CrowPanel Advance 4.3" HMI](https://github.com/Elecrow-RD/CrowPanel-Advance-4.3-HMI-ESP32-S3-AI-Powered-IPS-Touch-Screen-800x480)
(ESP32-S3-WROOM-1-N16R8, 800×480 IPS, hardware V1.1). It draws the aircraft around a point of your choice on a
radar scope over an OpenStreetMap coastline, with five selectable looks, route lookup (where a plane took off and
where it is heading) and a detail panel for the aircraft you tap.

Adapted from the [E-INK Live Flight Scanner](https://www.schematik.io/projects/build-me-flightscanner-live-ads-b-flight-radar-m-57ng)
project on Schematik (M5Stack PaperColor, e-ink) to the CrowPanel's colour RGB display. The radar centre is not
set in this repository: you enter your own position once (see [Setting your location](#setting-your-location)).

## What it does

- **Live traffic** from `opendata.adsb.fi`, with `api.adsb.lol` as fallback (both free, no key), every 15 s.
  Between two updates the aircraft are advanced along their speed and track, and the screen redraws every second.
  (`api.airplanes.live` and `adsb.one` are not used because they require prior approval, HTTP 403.)
- **Radar ranges** 25 / 50 / 100 km, chosen by touch.
- **Your place, your map**: the radar sits wherever you say. `tools/set_location.py` takes your position and builds
  the coastline, region border and runways around it from OpenStreetMap, embedded in the firmware. Land and sea
  can be filled in the themes that want it.
- **Route lookup** from the callsign (`api.adsbdb.com`, then the `vrs-standing-data` files of adsb.lol): the
  departure and arrival airports appear in the table as `FRA>AMS`. This is the scheduled route of that callsign,
  which is not always today's actual flight.
- **Tap an aircraft** (on the radar or in the table) for make and model, airline, route with country / city /
  short airport name, distance to go, ETA, altitude, speed, distance and bearing. The route is drawn on the radar,
  solid from the departure to the aircraft and dashed from the aircraft to the arrival.
- **Five themes** (below), switched by touch and remembered across reboots.
- **Remote screen monitor**: a small HTTP server for screenshots, status, virtual taps and debugging.

## Themes

Tap the **FLIGHTSCANNER** title (top left) to switch to the next theme. The choice is stored in NVS (namespace
`radar`), and the footer shows the theme name for three seconds. Remotely: `/action?name=theme&value=0..4`.

| | Theme | Look |
|---|---|---|
| A | Phosphor | Green phosphor radar scope: rotating sweep arm with a fading trail (one turn per 5 s), CRT scanlines, compass numbers every 10°. FreeMono font. |
| B | Flight map | Flight-tracker style: blue sea, dark green land. Aircraft colour follows altitude (orange to purple, 0-40k ft), with a trail of the last 10 positions and a callsign / FL / route tag. |
| C | Glass cockpit | Black background, white compass ring, cyan aircraft, magenta route. |
| D | Day chart | Light background, navy coastline, magenta region border, dark-outlined aircraft. For daylight. |
| E | ATC console | Air-traffic-control scope: diamond symbols, 1-minute speed vectors, trail dots, data blocks with leader lines (callsign, FL and speed, route) and 32 km approach lines for the airport's longest runway. Colour shows the role: arriving at the airport cyan, departing green, overflying white. FreeMono font. |


Notes:

- Themes are a table (`THEMES`) in `src/ui.cpp`: colours, font and drawing style (`Style::Arrow/Tag/Atc`) are chosen there.
- B and E fill land and sea from the land rings (`MAP_LAND`) that `tools/set_location.py` builds. OSM keeps land on the
  left of every coastline way, so the coast runs are joined along the 125 km keep circle and filled with the
  even-odd rule. The ATC approach lines come from the direction of the longest OSM runway near the airport.
- In theme A the sweep is not part of the full frame: after each frame the radar disc is copied to a buffer and
  the sweep wedge is blended straight into the on-screen framebuffer, step by step (about 16 ms), also while a frame
  is being drawn. A frame takes about 350 ms there against 190-220 ms in the other themes.
- FreeMono glyphs are 11 px wide, so in A and E the buttons read `100km`, airport names lose words such as
  `Airport` and the footer is compacted.
- Trails keep the last 10 fixes (15 s apart, about 2.5 min); an aircraft that has not been seen for 2 minutes is dropped.

## Hardware

| | |
|---|---|
| Board | Elecrow CrowPanel Advance 4.3" HMI, hardware revision **V1.1** |
| MCU | ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM) |
| Display | 4.3" IPS 800×480, 16-bit RGB, GT911 capacitive touch |
| Backlight | STC8H1K28 on I²C `0x30` (0 = brightest) |

Display pins and timing come from Elecrow's
[V1.1_and_V1.2_and_V1.3 example](https://github.com/Elecrow-RD/CrowPanel-Advance-4.3-HMI-ESP32-S3-AI-Powered-IPS-Touch-Screen-800x480/tree/master/example/V1.1_and_V1.2_and_V1.3).
Only V1.1 has been tested.

## Build and flash

1. Choose where the radar sits and build the map around it, see [Setting your location](#setting-your-location):

   ```
   python -I tools/set_location.py --lat <degrees> --lon <degrees> --tz "<time zone>"
   ```

   The build stops with a message until this is done.

2. Create your Wi-Fi file (2.4 GHz only). `include/secrets.h` is git-ignored:

   ```
   cp include/secrets.example.h include/secrets.h    # then edit it
   ```

   The name must match exactly (even `-` versus `_`). If the connection fails, the screen and the serial port list the
   networks the board can see.

3. Build and upload with [PlatformIO](https://platformio.org/):

   ```
   pio run -t upload
   pio device monitor
   ```

Notes:

- The project uses the [pioarduino](https://github.com/pioarduino/platform-espressif32) platform, which replaces the stock
  `espressif32` platform. If you use PlatformIO for other ESP32 projects, set `PLATFORMIO_CORE_DIR` to a separate
  folder before building so the global install is left alone.
- On Windows, build from a folder whose path has no non-ASCII characters (the linker cannot write its map file
  there), or point `PLATFORMIO_BUILD_DIR` at an ASCII path.
- On Windows, run the commands from PowerShell or the VS Code PlatformIO terminal. Git Bash (MSYS) breaks the
  pioarduino toolchain installation.

## Setting your location

Where the radar sits is up to you: **this repository contains no position at all**. One command, run once after
you download it (and again whenever you want to move the radar), sets it and builds the map around it. You need
Python 3, no extra packages:

```
python -I tools/set_location.py --lat 52.3731 --lon 4.8922 --tz "CET-1CEST,M3.5.0,M10.5.0/3"
```

| Option | Meaning |
|---|---|
| `--lat`, `--lon` | The centre in decimal degrees, south and west are negative (`--lon -74.006`). A neighbourhood is precise enough, two decimals are about 1 km. |
| `--name` | Label for the title bar instead of the automatic one (ASCII, at most 14 characters). By default the title bar shows the name of the place you chose: the nearest city, else town or village, from OpenStreetMap, in capitals without accents (`İzmir` becomes `IZMIR`). |
| `--tz` | POSIX time zone string for the clock. Default `UTC0`. Examples: `"UTC0"`, `"<+03>-3"`, `"CET-1CEST,M3.5.0,M10.5.0/3"`, `"GMT0BST,M3.5.0/1,M10.5.0"`, `"EST5EDT,M3.2.0,M11.1.0"`. |
| `--airport IATA`, `--airport none` | Reference airport instead of the nearest one, or none. |
| `--show-coords`, `--hide-coords` | Print the coordinates in the title bar or not (default not). |
| `--ocean` | You are over open water with no coast within 125 km: no land fill (otherwise a place without coastline is drawn as all land). |
| `--no-border` | Skip the region border (large regions download a lot). |

Run it without arguments to rebuild the map for the position that is already set; options you leave out keep their
current values. The tool works out the rest from OpenStreetMap through the public Overpass API (the servers are
sometimes overloaded, then it retries and tells you to try again later):

- the name of the place, for the title bar (the nearest city within 30 km, else the nearest town within 15 km,
  else village within 5 km; `HOME` when OpenStreetMap does not answer or you are out at sea, `--name` fixes that),
- the coastline, so land and sea can be filled,
- the nearest airport with an IATA code and an airliner-sized runway (`--airport` picks another one). It is drawn
  on the radar as a reference mark and decides the arrival / departure colours and the approach lines of the ATC theme,
- that airport's runways, and the border of the region (OSM admin level 4) you are in.

It writes two files, both **git-ignored**, so your position never ends up in a commit:

- `include/user_config.h`: position, label, reference airport and time zone. You can also create it by hand from
  `include/user_config.example.h`.
- `include/map_local.inc`: the map, which `src/mapdata.cpp` includes. Without it the radar simply has no coastline.

The build stops with a message until `user_config.h` exists. The map is embedded in the firmware, so after moving
the radar run `pio run -t upload` again. Positions beyond 80° of latitude or next to the antimeridian are not supported.

The map and the title bar show where the radar is (the coordinates only if you ask for them). Keep that in mind
before you publish screenshots, and never commit the two files above.

Your position also leaves the house in two ways. The radar sends it to the ADS-B feeds (adsb.fi, adsb.lol) every
15 s to ask for the aircraft around it, and `set_location.py` sends it to the Overpass servers once per run. If you
do not want those services to know exactly where you live, give a point a few kilometres off, for example a
neighbourhood centre: the radar works the same, it only moves everything by that distance.

## Usage

| Touch | Effect |
|---|---|
| Aircraft marker or table row | Selects the aircraft: the right panel switches from the list to the detail view and its route is drawn. Tap the marker again, an empty spot or the right panel to deselect. |
| 25 / 50 / 100 km | Radar range |
| REFRESH | Requests new data right away (at most every 3 s) |
| FLIGHTSCANNER title | Next theme |
| Top right (clock area) | Brightness: 100 % → 50 % → 15 % |

Serial monitor: `u` updates now, `i` prints the status line.

About routes:

- The route line passes through the aircraft: departure → aircraft and aircraft → arrival are separate great-circle
  arcs, not real airways. Where a route end lies outside the radar circle, the line is marked with the airport code
  at the point where it leaves the circle.
- If the aircraft is far from the listed route (the callsign was reused for another flight, or it deviates), the
  panel shows `route unsure` instead of an ETA.
- In the detail panel, departure and arrival are three lines under the code: country, city, short airport name
  ("Netherlands / Amsterdam / Schiphol"). Words like "International" and "Airport" and the city name are dropped from the
  airport name, at most 17 characters stay. The country name comes from the ISO code through a table in `src/route.cpp`,
  with the API's own name as fallback. The font is ASCII only, so names with accents are simplified.
- Aircraft that do not broadcast a callsign (private, some military) get no route lookup and show `--`.
- Route lookups run in the network task, one at a time between position fetches (about 1 s apart, the last 40
  callsigns are cached). The first 9 table rows and the selected aircraft are looked up, so a new aircraft's route
  appears a few seconds late.

## Remote monitoring

After connecting, the serial port prints `[monitor] http://<ip>/screenshot.bmp` (mDNS: `flightradar.local`).

- `/screenshot.bmp`: screenshot
- `/status`: status line
- `/action?name=tap&value=x,y`: virtual touch
- `/action?name=refresh`, `/action?name=theme&value=0..4`
- `/reboot`

**There is no authentication.** Anyone on your network can see the screen, tap it and reboot the board. Only use it on a
network you trust, and do not expose the board to the internet.

## Display shift (fixed) and debugging

Symptom: the picture jumped up by about 20 lines and the title bar showed up again below the footer, mostly after
pressing REFRESH (network and TLS activity). A screenshot cannot show it because the framebuffer is always right;
the corruption happens while it is scanned out to the panel.

Cause: the ESP-IDF RGB driver (bounce-buffer mode) picks which buffer to refill with `bb_eof_count % 2` and never resets
that counter when `CONFIG_LCD_RGB_RESTART_IN_VSYNC` is on. If the refill interrupt is late by more than one buffer
period, two events merge into one call, the counter stays odd and the picture stays one bounce buffer (20 lines)
off. Done in `src/board.cpp`:

- The counter is found at runtime and reset on every VSYNC, so a shift lasts one frame at most.
- The 768 KB cache write-back done on every frame was removed and `bb_invalidate_cache` is turned off (it delayed the refill interrupt).
- The pixel clock was lowered from 18 MHz to 14 MHz. Measured over 30 s with a REFRESH every 4 s: 20 late frames at
  18 MHz, 2 at 15 MHz, 0 at 14 MHz and below. The refresh rate is about 34 Hz.

Debugging over the monitor server (`http://<ip>/...`):

| Request | Effect |
|---|---|
| `/status` | Ends with `frame_ms=last/max` (draw time), `theme=`, `perf=` (time per drawing stage) and `scanout=vsync:N,bad:M,last:K,resync:1`. `bad` counts late frames, `last` is the event count of that frame (normally 24). |
| `/action?name=dbg&value=redraw:0` (or `1`) | Stops / resumes the 1 Hz redraw |
| `/action?name=dbg&value=net:0` (or `1`) | Stops / resumes the ADS-B requests |
| `/action?name=dbg&value=pclk:12000000` | Changes the pixel clock |
| `/action?name=dbg&value=txp:8` | Wi-Fi power in 0.25 dBm steps (8 = 2 dBm, 78 = 19.5 dBm) |
| `/action?name=dbg&value=label:TEXT` | Writes a label at the right end of the footer |

## Files

- `src/board.cpp`: RGB panel (double framebuffer + bounce buffer), GT911 touch, STC8H backlight (I²C 0x30).
- `src/traffic.cpp`: Wi-Fi, NTP, ADS-B fetching and JSON parsing (separate task on core 0).
- `src/route.cpp`: departure / arrival lookup from the callsign (adsbdb, fallback vrs-standing-data), cache, ASCII conversion.
- `src/mapdata.cpp`: includes the generated `include/map_local.inc` (coastline, border, runway and land polylines around your position), or an empty map when there is none.
- `tools/set_location.py`: sets the radar position (`include/user_config.h`) and builds the map (`include/map_local.inc`) from OpenStreetMap.
- `include/config.h`, `include/user_config.example.h`: constants (ranges, intervals) and the template for your own `user_config.h`.
- `include/mapdata.h`: the `MapKind` enum (coast, border, runway, land ring) and the data layout.
- `include/geo.h`: great-circle distance, bearing and the projection used for route drawing.
- `src/ui.cpp`: radar (the static background is rendered once per range and theme and cached), table, detail panel,
  buttons, themes, sweep (drawing with LovyanGFX).
- `src/monitor.cpp`, `src/screen_monitor.cpp`, `src/bmp565.cpp`: the remote monitor server (screenshot, status, virtual touch).
- `src/main.cpp`: touch and redraw loop.

## Data sources and licences

- Aircraft positions: [adsb.fi open data](https://adsb.fi/) and [adsb.lol](https://www.adsb.lol/).
- Routes: [adsbdb](https://www.adsbdb.com/) and the `vrs-standing-data` files served by [adsb.lol](https://vrs-standing-data.adsb.lol/).
- Map: © [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors, ODbL. The generated `include/map_local.inc` is a derived database,
  keep the attribution if you redistribute it.
- Libraries: [LovyanGFX](https://github.com/lovyan03/LovyanGFX) and [ArduinoJson](https://arduinojson.org/).

Please be considerate with these free services: the firmware makes one position request every 15 s and spreads
route lookups out.

## License

MIT, see [LICENSE](LICENSE).
