# ESP32-S3 IoT Weather Station

**A Wi-Fi weather and air-quality station on an ESP32-S3. It serves its own live dashboard and a JSON API.**

[![Project site](https://img.shields.io/badge/site-GitHub%20Pages-0ea5e9)](https://akshajtripathi24-ui.github.io/esp32-s3-weather-station/)
![Board](https://img.shields.io/badge/board-ESP32--S3-e7352c)
![Framework](https://img.shields.io/badge/framework-Arduino-00979d)
[![License: MIT](https://img.shields.io/badge/license-MIT-22c55e)](LICENSE)

The ESP32-S3 reads temperature, humidity, barometric pressure, gas level and fine dust. It hosts a dashboard that updates every 2 seconds and flags abnormal readings with a red banner and a short beep. You don't need a cloud service, an app or an account. Open the board's IP address in any browser on the same network.

🌐 **Project page:** https://akshajtripathi24-ui.github.io/esp32-s3-weather-station/

<!-- Add a photo of the build or a dashboard screenshot to /images and uncomment:
![Dashboard screenshot](images/dashboard.png)
-->

---

## Features

- **Temperature and humidity** from a DHT22, shown in °C and °F
- **Barometric pressure** in hPa from a BMP280 over I²C. The BMP280 is detected automatically at address `0x76` or `0x77`.
- **Air quality** from an MQ135, reported as the gas-sensor ratio Rs/Ro. The ratio is averaged over 32 ADC samples and smoothed.
- **Particulate matter** (PM2.5 and PM10, µg/m³) from a Plantower PMS5003 over UART. Each frame's checksum is verified.
- **Live web dashboard** at `/`. It refreshes every 2 s without reloading the page.
- **JSON API** at `/data` for Home Assistant, Node-RED, scripts or loggers
- **Alerts:** a card turns red and a banner appears when a reading leaves its safe range, and the browser plays a beep.
- **Status LED:** the onboard RGB NeoPixel shows boot, Wi-Fi and request state.
- **Credentials stay out of git:** Wi-Fi details live in a gitignored `secrets.h`.

### NeoPixel status colours

| Colour | Meaning |
|--------|---------|
| 🔴 Red | Booting and connecting to Wi-Fi |
| 🟣 Purple | Wi-Fi failed after about 20 s (40 tries × 500 ms) |
| 🟢 Green | Connected, idle |
| 🔵 Blue (flash) | Serving the dashboard page |

### Alert thresholds (dashboard)

| Reading | Alert when |
|---------|-----------|
| Temperature | > 45 °C or < 0 °C |
| Humidity | > 85 % or < 20 % |
| Pressure | < 550 hPa or > 1400 hPa |
| Air quality ratio | < 0.60 (lower Rs/Ro means more gas) |
| PM2.5 | > 65 µg/m³ |
| PM10 | > 150 µg/m³ |

---

## Parts list

| Qty | Part | Typical listing name | Notes |
|----|------|----------------------|-------|
| 1 | ESP32-S3 dev board | *ESP32-S3-DevKitC-1 N16R8* (or N8R2 / N8) | Onboard WS2812 RGB LED on GPIO48 |
| 1 | Temperature/humidity sensor | *DHT22 / AM2302 module* | Modules include the 10 kΩ pull-up |
| 1 | Pressure sensor | *GY-BMP280-3.3 breakout* | I²C address 0x76 (SDO→GND) or 0x77 |
| 1 | Gas sensor | *MQ-135 air quality sensor module* | Use the analog **AO** pin. Needs a 24–48 h burn-in. |
| 1 | Particulate sensor | *Plantower PMS5003 + G7/G5 adapter board* | 5 V supply, 3.3 V logic UART |
| 2 | Resistors (optional) | 10 kΩ + 20 kΩ | Voltage divider for the MQ135 AO (see [Wiring](#wiring)) |
| — | Breadboard, jumper wires, USB-C cable | | Use a data-capable USB cable |

---

## Wiring

All pin numbers come straight from [`firmware/weather_station/weather_station.ino`](firmware/weather_station/weather_station.ino).

| Sensor | Sensor pin | ESP32-S3 pin | Notes |
|--------|-----------|--------------|-------|
| **DHT22** | VCC | 3V3 | |
| | DATA | **GPIO4** | `#define DHTPIN 4` |
| | GND | GND | |
| **BMP280** | VCC | 3V3 | **3.3 V only** |
| | SDA | **GPIO8** | `Wire.begin(8, 9)` |
| | SCL | **GPIO9** | |
| | GND | GND | |
| **MQ135** | VCC | 5V | Heater needs 5 V |
| | AO | **GPIO1** (ADC1_CH0) | See the warning below |
| | GND | GND | |
| **PMS5003** | VCC (pin 1) | **5V** | The fan needs 5 V |
| | GND (pin 2) | GND | |
| | SET (pin 3) | leave floating or 3V3 | LOW = sleep |
| | **TX** (pin 5) | **GPIO15** (ESP32 RX) | Crossover: sensor TX → board RX |
| | **RX** (pin 4) | **GPIO7** (ESP32 TX) | Crossover: sensor RX → board TX |
| | RESET (pin 6) | leave floating | |
| **NeoPixel** | — | **GPIO48** | Already on the board |

> ⚠️ **MQ135 analog voltage.** The module runs at 5 V, so its AO pin can rise above the 3.3 V that ESP32-S3 GPIOs tolerate. The safest wiring is a **10 kΩ (top) + 20 kΩ (bottom) divider** between AO and GPIO1. If you use it, set `DIVIDER = 1.5` in the sketch. The sketch ships with `DIVIDER = 1.0`, which assumes AO is wired straight to GPIO1.

---

## Required Arduino libraries

| Library | Author | Used for |
|---------|--------|----------|
| **esp32** board package | Espressif Systems | `WiFi.h`, `WebServer.h`, `Wire.h`, `Serial1` |
| **DHT sensor library** | Adafruit | DHT22 |
| **Adafruit Unified Sensor** | Adafruit | Dependency of the DHT library |
| **Adafruit BMP280 Library** | Adafruit | BMP280 |
| **Adafruit BusIO** | Adafruit | Dependency of the BMP280 library |
| **Adafruit NeoPixel** | Adafruit | Onboard RGB LED |

### Install with the Arduino IDE (2.x)

1. **File → Preferences → Additional boards manager URLs** — add
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. **Tools → Board → Boards Manager**, search **esp32** and install **esp32 by Espressif Systems**.
3. **Tools → Manage Libraries** and install each of the following. Accept "Install all" when the IDE offers to add dependencies.
   - `DHT sensor library`
   - `Adafruit BMP280 Library`
   - `Adafruit NeoPixel`

### Or install with `arduino-cli`

```bash
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "DHT sensor library" "Adafruit Unified Sensor" "Adafruit BMP280 Library" "Adafruit BusIO" "Adafruit NeoPixel"
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
3. **Open** `firmware/weather_station/weather_station.ino` in the Arduino IDE.
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
   Connecting to Wi-Fi.....
   Connected! IP Address: http://192.168.1.42
   ```
3. Open that address in a browser on a phone or computer connected to the **same network**.
4. The cards fill in within 2 seconds and keep updating. Tap the page once so the browser allows the alert beep.

---

## `/data` JSON API

`GET http://<board-ip>/data` returns the current readings as `application/json`:

```json
{
  "tempC": 27.40,
  "tempF": 81.32,
  "humidity": 58.20,
  "pressure": 1008.63,
  "ratio": 1.12,
  "pm25": 18.00,
  "pm10": 27.00
}
```

| Field | Unit | Source | Notes |
|-------|------|--------|-------|
| `tempC` | °C | DHT22 | `0` if the DHT22 read fails |
| `tempF` | °F | DHT22 | `0` if the DHT22 read fails |
| `humidity` | % RH | DHT22 | `0` if the read fails |
| `pressure` | hPa | BMP280 | Falls back to **728.00** if the BMP280 returns nothing |
| `ratio` | Rs/Ro | MQ135 | Unitless. About 1.0 in clean air; lower means more gas. |
| `pm25` | µg/m³ | PMS5003 | Atmospheric-environment value. Stays `0` until the first valid frame. |
| `pm10` | µg/m³ | PMS5003 | Same as above |

Values are sent with 2 decimal places. Quick test:

```bash
curl http://192.168.1.42/data
```

---

## Troubleshooting

<details open>
<summary><b>PM2.5 and PM10 always read 0</b></summary>

The sketch only updates `pm25`/`pm10` after it receives a PMS5003 frame with a valid checksum, so a constant `0` means no good frames are arriving. Check these in order:

1. **TX/RX crossover.** Connect the sensor's **TX (pin 5) to ESP32 GPIO15 (RX)** and the sensor's **RX (pin 4) to ESP32 GPIO7 (TX)**. Connecting TX to TX is the most common mistake.
2. **5 V power.** Feed the PMS5003 from the **5V** pin, not 3V3. When it's powered you can hear and feel the fan spin. Make sure its GND is shared with the ESP32.
3. **Baud rate 9600.** The sensor's UART runs at 9600 8N1, which matches `Serial1.begin(9600, SERIAL_8N1, PMS_RX, PMS_TX)`. Don't change it.
4. **SET pin isn't pulled low.** If SET (pin 3) is LOW, the sensor sleeps: the fan stops and no data is sent. Leave it floating or tie it to 3V3.
5. **Watch the Serial Monitor.** Every valid frame prints `PM2.5=… PM10=…`. If those lines never appear, the problem is wiring or power, not the dashboard.
6. **Give it time.** After power-up, the fan needs about 30 s before readings are stable.
</details>

<details>
<summary><b>Pressure is stuck at 728 hPa</b></summary>

728.0 is the sketch's fallback value. The BMP280 isn't responding.
- Check SDA → **GPIO8** and SCL → **GPIO9**, and power it from **3V3**.
- Look for `Could not find a valid BMP280 sensor on 0x76 or 0x77!` at boot.
- Some boards sold as "BMP280" are actually **BME280** (chip ID 0x60) and fail `bmp.begin()`. Use the Adafruit BME280 library for those.
</details>

<details>
<summary><b>Temperature/humidity show 0</b></summary>

The DHT22 returned `NaN`. Check that DATA goes to **GPIO4** and VCC to 3V3. A bare 4-pin DHT22 (not a module) needs a 10 kΩ pull-up between DATA and VCC. Also keep requests to `/data` at least 2 s apart, because the DHT22 can't be read faster.
</details>

<details>
<summary><b>MQ135 ratio looks wrong or doesn't change</b></summary>

- Let a new sensor **burn in for 24–48 h**, and let it warm up for a few minutes after every power-on.
- `ro` (`31645.37`) is specific to one sensor. To recalibrate, put the warmed-up sensor in clean outdoor air, read the `Rs=` value from the Serial Monitor and set `ro` to it. After that the ratio reads about 1.0 in clean air and drops as gas levels rise.
- The ratio is relative, not ppm. Use it for trends and alerts, not absolute CO₂ values.
- Make sure `DIVIDER` matches your wiring (see the warning in [Wiring](#wiring)).
</details>

<details>
<summary><b>LED is purple / Wi-Fi won't connect</b></summary>

- Check that `secrets.h` exists next to the `.ino` and that the SSID and password are correct (both are case-sensitive).
- The ESP32-S3 supports **2.4 GHz only**, so it can't join a 5 GHz-only network.
- Move the board closer to the router, then press RST.
</details>

<details>
<summary><b>Nothing appears in the Serial Monitor</b></summary>

Set **Tools → USB CDC On Boot → Enabled**, select the right port and use **115200** baud. If the board has two USB-C ports, use the one labelled **USB**, or switch the setting and use **UART**.
</details>

<details>
<summary><b>Compile error: <code>secrets.h: No such file or directory</code></b></summary>

Copy `firmware/weather_station/secrets.example.h` to `secrets.h` in the same folder.
</details>

---

## Future ideas

- Read **PM1.0**. The PMS5003 already sends it in bytes 8–9 of each frame.
- Calculate a standard **AQI** from PM2.5/PM10 and colour the NeoPixel to match
- **Wi-Fi provisioning** (WiFiManager / captive portal) instead of `secrets.h`
- **mDNS** so the dashboard opens at `http://weather.local`
- **MQTT / Home Assistant** auto-discovery
- History charts with on-device storage (LittleFS) or a time-series database (InfluxDB)
- **OTA firmware updates**
- Deep sleep plus a battery and solar panel for outdoor use
- 3D-printed Stevenson screen enclosure
- Serve the dashboard from LittleFS instead of building it in a `String`

---

## Repository layout

```
.
├── firmware/
│   └── weather_station/
│       ├── weather_station.ino   # main sketch
│       └── secrets.example.h     # copy to secrets.h (gitignored)
├── docs/
│   └── index.html                # GitHub Pages project site
├── images/                       # photos & screenshots
├── LICENSE
└── README.md
```

## License

[MIT](LICENSE) © 2026 Akshaj Tripathi
