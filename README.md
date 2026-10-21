# ESP32-S3 IoT Weather Station

**A Wi-Fi weather and air-quality station on an ESP32-S3. It streams live readings over a WebSocket to its own dashboard.**

[![Project site](https://img.shields.io/badge/site-GitHub%20Pages-0ea5e9)](https://akshajtripathi24-ui.github.io/esp32-s3-weather-station/)
![Board](https://img.shields.io/badge/board-ESP32--S3-e7352c)
![Framework](https://img.shields.io/badge/framework-Arduino-00979d)
[![License: MIT](https://img.shields.io/badge/license-MIT-22c55e)](LICENSE)

The ESP32-S3 reads temperature, humidity, barometric pressure, gas level and fine dust (PM1.0, PM2.5, PM10). It pushes the readings once a second over a WebSocket to a dashboard it hosts itself, with live charts, alerts and a connection indicator. You don't need a cloud service, an app or an account. Open `http://weather.local` or the board's IP address in any browser on the same network.

The firmware runs a **fully non-blocking loop**: there are no `delay()` calls, every sensor runs on its own `millis()` timer, the PMS5003 is parsed byte by byte, and the async web server handles clients without stalling the sensors.

🌐 **Project page:** https://akshajtripathi24-ui.github.io/esp32-s3-weather-station/

<!-- Add a photo of the build or a dashboard screenshot to /images and uncomment:
![Dashboard screenshot](images/dashboard.png)
-->

---

## Features

- **Temperature and humidity** from a DHT22, shown in °C and °F
- **Barometric pressure** in hPa from a BMP280 over I²C. It's found automatically at `0x76` or `0x77` and re-detected if it drops out.
- **Air quality** from an MQ-135, reported as an estimated CO₂-equivalent **ppm** plus the raw Rs/Ro ratio
- **Particulate matter** PM1.0, PM2.5 and PM10 (µg/m³) from a Plantower PMS5003. Every frame's length and checksum are verified.
- **Live dashboard** at `/`: one card per reading with a sparkline, a 5-minute chart (tap a card to switch metric), a PM2.5 AQI label, alerts and an optional beep
- **Connection indicator** that shows the live ping time, notices a stalled stream and reconnects with exponential backoff (1 s, 2 s, 4 s … up to 10 s)
- **WebSocket stream** at `/ws` and a **JSON snapshot** at `/data` for Home Assistant, Node-RED or scripts
- **Honest data:** a sensor that stops answering is reported as `null` (shown as "--") instead of a made-up value
- **Wi-Fi resilience:** reconnects in the background, and the sensors keep running while Wi-Fi is down
- **mDNS:** reach the board at `http://weather.local`
- **Status LED:** the onboard NeoPixel shows connection state
- **Credentials stay out of git:** Wi-Fi details live in a gitignored `secrets.h`

### NeoPixel status colours

| Colour | Meaning |
|--------|---------|
| 🔴 Red | Booting and connecting to Wi-Fi |
| 🟣 Purple | No Wi-Fi after 20 s, or the connection dropped. It keeps retrying in the background. |
| 🟢 Green | Connected |
| 🔵 Blue (flash) | Serving the dashboard page |

### Alert thresholds (dashboard)

| Reading | Alert when |
|---------|-----------|
| Temperature | > 45 °C or < 0 °C |
| Humidity | > 85 % or < 20 % |
| Pressure | < 550 hPa or > 1400 hPa |
| Air quality | > 1600 ppm CO₂-eq (Rs/Ro below about 0.6) |
| PM2.5 | > 65 µg/m³ |
| PM10 | > 150 µg/m³ |

---

## Parts list

| Qty | Part | Typical listing name | Notes |
|----|------|----------------------|-------|
| 1 | ESP32-S3 dev board | [ESP32-S3-DevKitC-1 with USB-C cable](https://www.amazon.com/dp/B0H7HQJ4NW) ([2-pack](https://www.amazon.com/dp/B0H7HVBNVQ)) | Onboard WS2812 RGB LED on GPIO48 |
| 1 | Temperature/humidity sensor | [DHT22 module](https://www.amazon.com/dp/B0GSR68YH9) (sold as a 2-pack) | Modules include the 10 kΩ pull-up |
| 1 | Pressure sensor | [BMP280 module, pre-soldered](https://www.amazon.com/dp/B0CD4PQZGQ) | I²C address 0x76 (SDO→GND) or 0x77 |
| 1 | Gas sensor | [MQ-135 air quality module](https://www.amazon.com/dp/B0DQHJFCLD) | Use the analog **AO** pin. Needs a 24–48 h burn-in. |
| 1 | Particulate sensor | [PMS5003 with adapter cable](https://www.amazon.com/dp/B0B1DQKV4N) | 5 V supply, 3.3 V logic UART |
| 1 | Power module | *HW-131 breadboard power supply* | Dedicated 5 V rail for the high-draw parts |
| 1 | Breadboard + jumper wires | [Breadboard kit, M-M / M-F / F-F wires](https://www.amazon.com/dp/B09VKYLYN7) | Full-size and half-size boards |
| 2 | Resistors | 10 kΩ + 20 kΩ | Voltage divider for the MQ-135 AO (see [Wiring](#wiring)) |

---

## Wiring

All pin numbers come from [`firmware/weather_station/weather_station.ino`](firmware/weather_station/weather_station.ino).

### Power

| Rail | Source | Feeds |
|------|--------|-------|
| **5V rail** | **HW-131** power module | PMS5003 (fan and laser), MQ-135 heater, DHT22 |
| 3V3 | ESP32-S3 3V3 pin | BMP280 |
| ESP32-S3 | USB-C | The board itself |
| **GND** | Common | Connect HW-131 GND to ESP32-S3 GND |

> ⚠️ When the HW-131 powers the 5V rail, **don't also connect the ESP32-S3's 5V pin to it**. Two supplies on one rail can push current into each other. Only the grounds should be shared.

### Signals

| Sensor | Sensor pin | ESP32-S3 pin | Notes |
|--------|-----------|--------------|-------|
| **DHT22** | DATA | **GPIO4** | VCC from the 5V rail |
| **BMP280** | SDA | **GPIO8** | VCC from 3V3 only |
| | SCL | **GPIO9** | |
| **MQ-135** | AO | **GPIO1** (ADC1_CH0) | Through the divider, see below |
| **PMS5003** | **TX** (pin 5) | **GPIO15** (ESP32 RX) | Crossover: sensor TX → board RX |
| | **RX** (pin 4) | **GPIO7** (ESP32 TX) | Crossover: sensor RX → board TX |
| | SET (pin 3) | leave floating or 3V3 | LOW = sleep |
| | RESET (pin 6) | leave floating | |
| **NeoPixel** | — | **GPIO48** | Already on the board |

> ⚠️ **MQ-135 analog voltage.** The module runs at 5 V, so its AO pin can rise above the 3.3 V that ESP32-S3 GPIOs tolerate. Put a **10 kΩ (top) + 20 kΩ (bottom) divider** between AO and GPIO1 and set `DIVIDER = 1.5` in the sketch. The sketch ships with `DIVIDER = 1.0`, which means AO is wired straight to GPIO1.

---

## Required Arduino libraries

| Library | Author | Used for |
|---------|--------|----------|
| **esp32** board package (3.x) | Espressif Systems | `WiFi.h`, `ESPmDNS.h`, `Wire.h`, `Serial1` |
| **ESP Async WebServer** | ESP32Async | Async HTTP server and WebSocket (`/ws`) |
| **Async TCP** | ESP32Async | Dependency of ESP Async WebServer |
| **DHT sensor library** | Adafruit | DHT22 |
| **Adafruit Unified Sensor** | Adafruit | Dependency of the DHT library |
| **Adafruit BMP280 Library** | Adafruit | BMP280 |
| **Adafruit BusIO** | Adafruit | Dependency of the BMP280 library |
| **Adafruit NeoPixel** | Adafruit | Onboard RGB LED |

> Use the **ESP32Async** versions of ESP Async WebServer and Async TCP. The old `me-no-dev` versions don't build on esp32 core 3.x.

### Install with the Arduino IDE (2.x)

1. **File → Preferences → Additional boards manager URLs** — add
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. **Tools → Board → Boards Manager**, search **esp32** and install **esp32 by Espressif Systems** (3.x).
3. **Tools → Manage Libraries** and install each of the following. Accept "Install all" when the IDE offers to add dependencies.
   - `ESP Async WebServer` (by ESP32Async)
   - `Async TCP` (by ESP32Async)
   - `DHT sensor library`
   - `Adafruit BMP280 Library`
   - `Adafruit NeoPixel`

### Or install with `arduino-cli`

```bash
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "ESP Async WebServer" "Async TCP" "DHT sensor library" "Adafruit Unified Sensor" "Adafruit BMP280 Library" "Adafruit BusIO" "Adafruit NeoPixel"
```

---

## Flashing

1. **Clone the repo**
   ```bash
   git clone https://github.com/akshajtripathi24-ui/esp32-s3-weather-station.git
   cd esp32-s3-weather-station/firmware/weather_station
   ```
2. **Add your Wi-Fi credentials.** Copy `secrets.example.h` to `secrets.h` and edit it:
   ```cpp
   #define WIFI_SSID     "your-wifi-name"
   #define WIFI_PASSWORD "your-wifi-password"
   ```
   `secrets.h` is gitignored, so it never gets committed. The ESP32-S3 joins **2.4 GHz** networks only.
3. **Open** `firmware/weather_station/weather_station.ino` in the Arduino IDE. `dashboard.h` opens as a second tab.
4. **Board settings** (Tools menu):
   - Board: **ESP32S3 Dev Module**
   - USB CDC On Boot: **Enabled**. This sends `Serial` output to the USB-C port.
   - Flash size / PSRAM: match your module (for example 16 MB / OPI PSRAM for N16R8)
   - Port: the COM / `/dev/tty*` port of the board
5. Click **Upload**. If the upload doesn't start, hold **BOOT**, tap **RST**, then release **BOOT**.

With `arduino-cli`:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc firmware/weather_station
arduino-cli upload  --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc -p COM5 firmware/weather_station
```

---

## Opening the dashboard

1. Open the **Serial Monitor** at **115200 baud** and press **RST**.
2. Wait for:
   ```
   Connecting to Wi-Fi...
   Connected! IP Address: http://192.168.1.42  (or http://weather.local)
   ```
3. Open **http://weather.local** or that IP address in a browser on the **same network**. Android browsers may not resolve `.local` names, so use the IP address there.
4. The status pill shows **Live · NN ms** while data is streaming. Tap a card to chart it, and turn on **Sound** if you want a beep when an alert starts.

Every 5 seconds the Serial Monitor also prints a status line with all readings, the MQ-135 resistance and the PMS5003 good/bad frame counts.

Want to try the dashboard without hardware? Extract the HTML from `dashboard.h`, open it with `?demo` at the end of the URL, and it runs on sample data.

---

## Data API

### WebSocket: `ws://<board>/ws`

- The server pushes one JSON text frame per second, plus one straight away when a client connects.
- Send `ping:<anything>` and the server replies `pong:<anything>`. The dashboard sends `ping:<Date.now()>` every 5 s to measure round-trip time.

### HTTP: `GET http://<board>/data`

This returns the same JSON as a one-off snapshot, with `Access-Control-Allow-Origin: *`.

```json
{
  "tempC": 27.40,
  "tempF": 81.32,
  "humidity": 58.20,
  "pressure": 1008.63,
  "ratio": 1.093,
  "ppm": 329,
  "pm1": 11,
  "pm25": 18,
  "pm10": 27,
  "uptime": 3725,
  "rssi": -58,
  "clients": 1
}
```

| Field | Unit | Source | Notes |
|-------|------|--------|-------|
| `tempC`, `tempF` | °C / °F | DHT22 | `null` after 3 failed reads in a row |
| `humidity` | % RH | DHT22 | Same as above |
| `pressure` | hPa | BMP280 | `null` while the BMP280 is missing |
| `ratio` | Rs/Ro | MQ-135 | About 1.0 in clean air; lower means more gas |
| `ppm` | ppm CO₂-eq | MQ-135 | **Estimate** from the datasheet CO₂ curve, not a calibrated CO₂ reading |
| `pm1`, `pm25`, `pm10` | µg/m³ | PMS5003 | Atmospheric-environment values. `null` if no valid frame in 10 s. |
| `uptime` | s | ESP32-S3 | Seconds since boot |
| `rssi` | dBm | Wi-Fi | `0` when disconnected |
| `clients` | count | WebSocket | Dashboards currently connected |

```bash
curl http://weather.local/data
```

---

## Firmware design

| Task | How it avoids blocking |
|------|------------------------|
| PMS5003 | A byte-by-byte state machine (`0x42 0x4D` header → 30-byte body) that reads at most 64 bytes per loop pass and checks the length and checksum |
| MQ-135 | One ADC sample every 10 ms, averaged over 32 samples. The old version took 32 samples in a burst with `delay(2)`. |
| DHT22 | Read every 2 s, its maximum rate. Up to 2 failed reads in a row are ignored. |
| BMP280 | Read every 1 s, re-detected every 10 s if it's missing |
| Web | ESPAsyncWebServer handles HTTP and WebSocket on its own task. A shared-state lock (`portMUX`) keeps the JSON consistent between that task and `loop()`. |
| Broadcast | A compact JSON is built with `snprintf` into a stack buffer (no `String` allocation). A frame is skipped rather than queued when a client is slow. |
| Wi-Fi | Auto-reconnect plus a manual retry every 15 s. The sensors keep running while Wi-Fi is down. |

---

## Build log: problems we solved

| Problem | Cause | Fix |
|---------|-------|-----|
| **Unstable MQ-135 readings** | The heater didn't get a steady 5 V while sharing the microcontroller's rails | The heater runs from the dedicated HW-131 5 V rail, and the AO signal goes through a divider to stay safe for the 3.3 V ADC |
| **PMS5003 brownouts and resets** | The fan and laser draw current spikes that sagged the board's 5 V (on one board the 5V pin failed outright) | The HW-131 power module provides a separate, sturdy 5 V rail with a shared ground |
| **Dashboard data drops** | `delay()` and blocking serial reads stalled the loop, so sensor frames and web updates were lost | Rebuilt as a non-blocking loop: `millis()` timers, a byte-wise PMS5003 state machine, an async WebSocket push and `snprintf` JSON |

---

## Troubleshooting

<details open>
<summary><b>PM2.5 and PM10 always read 0 or "--"</b></summary>

The sketch only updates the PM values after it receives a PMS5003 frame with a valid checksum, and it reports `null` ("--") if none arrives for 10 s. Check these in order:

1. **TX/RX crossover.** Connect the sensor's **TX (pin 5) to ESP32 GPIO15 (RX)** and the sensor's **RX (pin 4) to ESP32 GPIO7 (TX)**. Connecting TX to TX is the most common mistake.
2. **5 V power.** Feed the PMS5003 from the **HW-131 5V rail**, not 3V3. When it's powered you can hear and feel the fan spin. Make sure its GND is shared with the ESP32.
3. **Baud rate 9600.** The sensor's UART runs at 9600 8N1, which matches `Serial1.begin(9600, SERIAL_8N1, PMS_RX, PMS_TX)`. Don't change it.
4. **SET pin isn't pulled low.** If SET (pin 3) is LOW, the sensor sleeps: the fan stops and no data is sent. Leave it floating or tie it to 3V3.
5. **Watch the Serial Monitor.** The 5-second status line shows `frames ok=… bad=…`. If `ok` never goes up, the problem is wiring or power. If `bad` keeps rising, look for a loose wire or noise on the RX line.
6. **Give it time.** After power-up, the fan needs about 30 s before readings are stable.
</details>

<details>
<summary><b>Dashboard says "Reconnecting" or "Offline"</b></summary>

- The browser lost the WebSocket. It retries by itself with backoff, so no reload is needed.
- Check the board's LED. Purple means the board itself lost Wi-Fi; it retries every 15 s.
- If `weather.local` doesn't open, use the IP address from the Serial Monitor. Android and some networks don't support mDNS.
- Each open dashboard tab is one WebSocket client, and ESPAsyncWebServer handles a handful comfortably.
</details>

<details>
<summary><b>Pressure shows "--"</b></summary>

The BMP280 isn't responding. The firmware retries every 10 s and prints `BMP280 not found…`.
- Check SDA → **GPIO8** and SCL → **GPIO9**, and power it from **3V3**.
- Some boards sold as "BMP280" are actually **BME280** (chip ID 0x60) and fail `bmp.begin()`. Use the Adafruit BME280 library for those.
</details>

<details>
<summary><b>Temperature/humidity show "--"</b></summary>

The DHT22 failed 3 reads in a row. Check that DATA goes to **GPIO4** and VCC to the 5V rail. A bare 4-pin DHT22 (not a module) needs a 10 kΩ pull-up between DATA and VCC.
</details>

<details>
<summary><b>Air-quality ppm looks wrong</b></summary>

- Let a new sensor **burn in for 24–48 h**, and let it warm up for a few minutes after every power-on.
- `ro` (`31645.37`) is specific to one sensor. To recalibrate, put the warmed-up sensor in clean outdoor air and read `MQ Rs=` from the 5-second status line. Set `ro` to that value. After that, clean air reads about 420 ppm and the ratio about 1.0.
- The ppm value is a CO₂-equivalent estimate from the datasheet curve. The MQ-135 reacts to many gases, so use it for trends and alerts, not as a calibrated CO₂ meter.
- Make sure `DIVIDER` matches your wiring (see [Wiring](#wiring)).
</details>

<details>
<summary><b>Compile errors</b></summary>

- `secrets.h: No such file or directory`: copy `secrets.example.h` to `secrets.h` in the same folder.
- Errors inside `AsyncTCP` or `ESPAsyncWebServer`: you have the old `me-no-dev` libraries. Remove them and install the **ESP32Async** versions.
- `ESPmDNS.h` not found: install the esp32 board package 3.x and select an ESP32 board.
</details>

<details>
<summary><b>Nothing appears in the Serial Monitor</b></summary>

Set **Tools → USB CDC On Boot → Enabled**, select the right port and use **115200** baud. If the board has two USB-C ports, use the one labelled **USB**, or switch the setting and use **UART**.
</details>

---

## Future ideas

- Add a **UV sensor** (VEML6075) for the outdoor UV-A/UV-B index
- Calculate a full **US AQI** number from PM2.5/PM10 and colour the NeoPixel to match
- **Wi-Fi provisioning** (WiFiManager / captive portal) instead of `secrets.h`
- **MQTT / Home Assistant** auto-discovery
- Longer history stored on the device (LittleFS) or in InfluxDB
- **OTA firmware updates**
- Deep sleep plus a battery and solar panel for outdoor use
- 3D-printed Stevenson screen enclosure

---

## Repository layout

```
.
├── firmware/
│   └── weather_station/
│       ├── weather_station.ino   # non-blocking main sketch
│       ├── dashboard.h           # dashboard HTML/CSS/JS served at "/"
│       └── secrets.example.h     # copy to secrets.h (gitignored)
├── docs/
│   └── index.html                # GitHub Pages project site
├── images/                       # photos & screenshots
├── LICENSE
└── README.md
```

## License

[MIT](LICENSE) © 2026 Akshaj Tripathi
