// ESP32-S3 IoT Weather Station
// DHT22 + BMP280 + MQ-135 + PMS5003 with a fully non-blocking main loop.
//   /      live dashboard (served from dashboard.h)
//   /ws    WebSocket stream, one JSON frame per second
//   /data  JSON snapshot for scripts and integrations
// Wi-Fi credentials live in secrets.h (copy secrets.example.h to get started).

#include <WiFi.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DHT.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_NeoPixel.h>
#include "secrets.h"
#include "dashboard.h"

// ---------- pins ----------
#define DHTPIN     4
#define DHTTYPE    DHT22
#define I2C_SDA    8
#define I2C_SCL    9
#define MQ135_PIN  1
#define PMS_RX     15   // ESP32 RX <- PMS5003 TX
#define PMS_TX     7    // ESP32 TX -> PMS5003 RX
#define RGB_PIN    48
#define NUM_PIXELS 1

const char* HOSTNAME = "weather";   // dashboard also answers at http://weather.local

// ---------- timing (ms) ----------
const uint32_t DHT_INTERVAL        = 2000;   // the DHT22 can't be read faster than this
const uint32_t BMP_INTERVAL        = 1000;
const uint32_t BMP_RETRY_INTERVAL  = 10000;  // re-detect a missing BMP280
const uint32_t MQ_SAMPLE_INTERVAL  = 10;     // one ADC sample per tick, averaged in batches
const uint8_t  MQ_SAMPLES          = 32;
const uint32_t PMS_STALE_AFTER     = 10000;  // report PM as null if no valid frame for this long
const uint32_t BROADCAST_INTERVAL  = 1000;
const uint32_t WIFI_RETRY_INTERVAL = 15000;
const uint32_t WIFI_GIVE_UP_LED    = 20000;  // LED turns purple after this long without Wi-Fi
const uint32_t LOG_INTERVAL        = 5000;
const uint8_t  DHT_MAX_FAILS       = 3;      // consecutive bad reads before reporting null

// ---------- MQ-135 ----------
const float SENSOR_VCC = 5.0;    // voltage powering the MQ-135 module
const float DIVIDER    = 1.0;    // 1.0 = AO wired straight to GPIO1; 1.5 = 10k + 20k divider
const float RL         = 20000.0;
float ro               = 31645.37;  // Rs in clean air; recalibrate per sensor (see README)
const float ATMO_CO2   = 420.0;     // ppm of CO2 in clean outdoor air
const float MQ_CURVE_B = -2.769034857;  // CO2 curve slope from the MQ-135 datasheet

// ---------- objects ----------
Adafruit_NeoPixel pixels(NUM_PIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);
DHT dht(DHTPIN, DHTTYPE);
Adafruit_BMP280 bmp;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ---------- shared state ----------
// Written by loop(), read by the async web task, so access goes through liveMux.
struct Readings {
  float tempC    = NAN;
  float humidity = NAN;
  float pressure = NAN;
  float ratio    = NAN;
  float ppm      = NAN;
  float pm1      = NAN;
  float pm25     = NAN;
  float pm10     = NAN;
  uint32_t pmsAt = 0;   // millis() of the last valid PMS5003 frame
};
Readings live;
portMUX_TYPE liveMux = portMUX_INITIALIZER_UNLOCKED;

bool bmpOk = false;
bool wifiUp = false;
uint32_t pmsGoodFrames = 0;
uint32_t pmsBadFrames = 0;
float mqRs = NAN;
volatile uint32_t ledFlashUntil = 0;

const size_t JSON_SIZE = 320;

Readings snapshot() {
  portENTER_CRITICAL(&liveMux);
  Readings r = live;
  portEXIT_CRITICAL(&liveMux);
  return r;
}

// Formats a number, or "null" so the dashboard can show "--" for a failed sensor.
const char* num(char* buf, float v, int decimals) {
  if (isnan(v)) return "null";
  snprintf(buf, 16, "%.*f", decimals, v);
  return buf;
}

size_t buildJson(char* out, size_t size) {
  Readings r = snapshot();
  bool pmsFresh = r.pmsAt != 0 && millis() - r.pmsAt < PMS_STALE_AFTER;
  float tempF = isnan(r.tempC) ? NAN : r.tempC * 1.8f + 32.0f;

  char a[16], b[16], c[16], d[16], e[16], f[16], g[16], h[16], i[16];
  return snprintf(out, size,
    "{\"tempC\":%s,\"tempF\":%s,\"humidity\":%s,\"pressure\":%s,"
    "\"ratio\":%s,\"ppm\":%s,\"pm1\":%s,\"pm25\":%s,\"pm10\":%s,"
    "\"uptime\":%lu,\"rssi\":%d,\"clients\":%u}",
    num(a, r.tempC, 2), num(b, tempF, 2), num(c, r.humidity, 2), num(d, r.pressure, 2),
    num(e, r.ratio, 3), num(f, r.ppm, 0),
    num(g, pmsFresh ? r.pm1 : NAN, 0), num(h, pmsFresh ? r.pm25 : NAN, 0), num(i, pmsFresh ? r.pm10 : NAN, 0),
    (unsigned long)(millis() / 1000), wifiUp ? (int)WiFi.RSSI() : 0, (unsigned)ws.count());
}

// ---------- PMS5003: byte-by-byte state machine, never blocks ----------
void pollPMS5003() {
  static uint8_t frame[32];
  static uint8_t pos = 0;
  int budget = 64;   // cap bytes per pass so the loop stays responsive

  while (Serial1.available() && budget-- > 0) {
    uint8_t byteIn = Serial1.read();

    if (pos == 0 && byteIn != 0x42) continue;
    if (pos == 1 && byteIn != 0x4D) {
      pos = (byteIn == 0x42) ? 1 : 0;   // a stray 0x42 may be the start of the next frame
      continue;
    }
    frame[pos++] = byteIn;
    if (pos < sizeof(frame)) continue;
    pos = 0;

    uint16_t length = (frame[2] << 8) | frame[3];
    uint16_t sum = 0;
    for (int k = 0; k < 30; k++) sum += frame[k];
    uint16_t checksum = (frame[30] << 8) | frame[31];
    if (length != 28 || sum != checksum) {
      pmsBadFrames++;
      continue;
    }

    // Atmospheric-environment values (frame bytes 10-15)
    portENTER_CRITICAL(&liveMux);
    live.pm1   = (frame[10] << 8) | frame[11];
    live.pm25  = (frame[12] << 8) | frame[13];
    live.pm10  = (frame[14] << 8) | frame[15];
    live.pmsAt = millis();
    portEXIT_CRITICAL(&liveMux);
    pmsGoodFrames++;
  }
}

// ---------- MQ-135: one sample per tick instead of a blocking burst ----------
void pollMQ135(uint32_t now) {
  static uint32_t last = 0;
  static uint32_t sum = 0;
  static uint8_t count = 0;
  static float smooth = -1;

  if (now - last < MQ_SAMPLE_INTERVAL) return;
  last = now;

  sum += analogReadMilliVolts(MQ135_PIN);
  if (++count < MQ_SAMPLES) return;

  float vOut = (sum / (float)MQ_SAMPLES) * DIVIDER / 1000.0f;   // volts at the sensor's AO pin
  sum = 0;
  count = 0;
  if (vOut < 0.01f) return;   // no signal, keep the last value
  if (vOut > SENSOR_VCC - 0.01f) vOut = SENSOR_VCC - 0.01f;

  float rs = RL * ((SENSOR_VCC - vOut) / vOut);
  float r = rs / ro;
  smooth = (smooth < 0) ? r : 0.9f * smooth + 0.1f * r;
  mqRs = rs;

  portENTER_CRITICAL(&liveMux);
  live.ratio = smooth;
  live.ppm = ATMO_CO2 * powf(smooth, MQ_CURVE_B);   // CO2-equivalent estimate
  portEXIT_CRITICAL(&liveMux);
}

// ---------- DHT22 ----------
void pollDHT(uint32_t now) {
  static uint32_t last = 0;
  static uint8_t fails = 0;
  if (now - last < DHT_INTERVAL) return;
  last = now;

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  bool ok = !isnan(t) && !isnan(h);
  if (ok) fails = 0;
  else if (fails < DHT_MAX_FAILS) fails++;

  // Ride out an occasional bad read, report null once the sensor is really gone
  if (!ok && fails < DHT_MAX_FAILS) return;
  portENTER_CRITICAL(&liveMux);
  live.tempC = ok ? t : NAN;
  live.humidity = ok ? h : NAN;
  portEXIT_CRITICAL(&liveMux);
}

// ---------- BMP280 ----------
bool beginBMP() {
  if (bmp.begin(0x76)) { Serial.println("BMP280 found at 0x76"); return true; }
  if (bmp.begin(0x77)) { Serial.println("BMP280 found at 0x77"); return true; }
  return false;
}

void pollBMP(uint32_t now) {
  static uint32_t last = 0;
  static uint32_t lastRetry = 0;

  if (!bmpOk) {
    if (now - lastRetry < BMP_RETRY_INTERVAL) return;
    lastRetry = now;
    bmpOk = beginBMP();
    if (!bmpOk) Serial.println("BMP280 not found on 0x76 or 0x77, retrying in 10 s");
    return;
  }

  if (now - last < BMP_INTERVAL) return;
  last = now;

  float pa = bmp.readPressure();
  bool valid = !isnan(pa) && pa > 30000 && pa < 120000;
  portENTER_CRITICAL(&liveMux);
  live.pressure = valid ? pa / 100.0f : NAN;
  portEXIT_CRITICAL(&liveMux);
  if (!valid) bmpOk = false;   // re-detect on the next retry
}

// ---------- Wi-Fi: reconnect in the background ----------
void pollWiFi(uint32_t now) {
  static bool wasUp = false;
  static bool mdnsStarted = false;
  static uint32_t lastAttempt = 0;

  wifiUp = WiFi.status() == WL_CONNECTED;

  if (wifiUp && !wasUp) {
    Serial.printf("Connected! IP Address: http://%s  (or http://%s.local)\n",
                  WiFi.localIP().toString().c_str(), HOSTNAME);
    if (!mdnsStarted && MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
    }
  }
  if (!wifiUp && wasUp) {
    Serial.println("Wi-Fi lost, reconnecting...");
    lastAttempt = now;
  }
  if (!wifiUp && now - lastAttempt >= WIFI_RETRY_INTERVAL) {
    lastAttempt = now;
    WiFi.reconnect();
  }
  wasUp = wifiUp;
}

// ---------- WebSocket ----------
void broadcast(uint32_t now) {
  static uint32_t last = 0;
  if (now - last < BROADCAST_INTERVAL) return;
  last = now;

  ws.cleanupClients();
  if (ws.count() == 0 || !ws.availableForWriteAll()) return;   // skip a frame rather than queue up

  char json[JSON_SIZE];
  buildJson(json, sizeof(json));
  ws.textAll(json);
}

void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type,
               void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    char json[JSON_SIZE];
    buildJson(json, sizeof(json));
    client->text(json);   // new clients get data right away
  } else if (type == WS_EVT_DATA) {
    // Echo "ping:<t>" as "pong:<t>" so the dashboard can show round-trip time
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT &&
        len > 5 && len < 40 && memcmp(data, "ping:", 5) == 0) {
      char reply[48] = "pong:";
      memcpy(reply + 5, data + 5, len - 5);
      reply[len] = '\0';
      client->text(reply);
    }
  }
}

// ---------- status LED ----------
// Red: connecting, purple: Wi-Fi down, green: online, blue flash: dashboard served
void updateLed(uint32_t now) {
  static uint32_t shown = 0xFFFFFFFF;
  static bool everConnected = false;
  uint32_t color;
  if (wifiUp) {
    everConnected = true;
    color = ((int32_t)(ledFlashUntil - now) > 0) ? pixels.Color(0, 0, 255) : pixels.Color(0, 250, 0);
  } else if (!everConnected && now < WIFI_GIVE_UP_LED) {
    color = pixels.Color(255, 0, 0);
  } else {
    color = pixels.Color(128, 0, 128);
  }
  if (color == shown) return;
  shown = color;
  pixels.setPixelColor(0, color);
  pixels.show();
}

void logStatus(uint32_t now) {
  static uint32_t last = 0;
  if (now - last < LOG_INTERVAL) return;
  last = now;
  Readings r = snapshot();
  Serial.printf("T=%.1fC H=%.1f%% P=%.1fhPa | MQ Rs=%.0f ratio=%.2f ~%.0fppm | PM1=%.0f PM2.5=%.0f PM10=%.0f "
                "(frames ok=%lu bad=%lu) | WiFi=%s clients=%u\n",
                r.tempC, r.humidity, r.pressure, mqRs, r.ratio, r.ppm, r.pm1, r.pm25, r.pm10,
                (unsigned long)pmsGoodFrames, (unsigned long)pmsBadFrames,
                wifiUp ? "up" : "down", (unsigned)ws.count());
}

void setup() {
  Serial.begin(115200);
  Serial1.setRxBufferSize(256);
  Serial1.begin(9600, SERIAL_8N1, PMS_RX, PMS_TX);

  pixels.begin();
  pixels.setBrightness(30);
  pixels.setPixelColor(0, pixels.Color(255, 0, 0));
  pixels.show();

  pinMode(MQ135_PIN, INPUT);
  analogSetAttenuation(ADC_11db);

  dht.begin();
  Wire.begin(I2C_SDA, I2C_SCL);
  bmpOk = beginBMP();
  if (!bmpOk) Serial.println("BMP280 not found on 0x76 or 0x77, retrying in 10 s");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("Connecting to Wi-Fi...");

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    ledFlashUntil = millis() + 150;
    request->send(200, "text/html; charset=utf-8", (const uint8_t*)DASHBOARD_HTML, strlen(DASHBOARD_HTML));
  });

  server.on("/data", HTTP_GET, [](AsyncWebServerRequest* request) {
    char json[JSON_SIZE];
    buildJson(json, sizeof(json));
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });

  server.onNotFound([](AsyncWebServerRequest* request) {
    request->send(404, "text/plain", "Not found");
  });

  server.begin();
}

void loop() {
  uint32_t now = millis();
  pollWiFi(now);
  pollPMS5003();
  pollMQ135(now);
  pollDHT(now);
  pollBMP(now);
  broadcast(now);
  updateLed(now);
  logStatus(now);
}
