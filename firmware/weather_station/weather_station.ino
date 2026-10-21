// ESP32-S3 IoT Weather Station
// DHT22 + BMP280 + MQ135 + PMS5003, live dashboard at "/" and JSON at "/data".
// Wi-Fi credentials live in secrets.h (copy secrets.example.h to get started).

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <DHT.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_NeoPixel.h>
#include "secrets.h"

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

WebServer server(80);

#define DHTPIN 4
#define DHTTYPE DHT22
const int mq135Pin = 1;

#define RGB_PIN 48
#define NUM_PIXELS 1
Adafruit_NeoPixel pixels(NUM_PIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);

DHT dht(DHTPIN, DHTTYPE);
Adafruit_BMP280 bmp;

const int PMS_RX = 15;
const int PMS_TX = 7;
float pm25 = 0;
float pm10 = 0;

const float SENSOR_VCC = 5.0;   // voltage powering the MQ135 module
const float DIVIDER    = 1.0;   // 1.0 = AO wired straight to GPIO1; 1.5 = 10k + 20k divider
const float rl = 20000.0;
float ro = 31645.37;            // recalibrate, see below

float readMq135Ratio() {
  uint32_t sum = 0;
  for (int i = 0; i < 32; i++) { sum += analogReadMilliVolts(mq135Pin); delay(2); }
  float vOut = (sum / 32.0) * DIVIDER / 1000.0;   // volts at the sensor's AO pin
  if (vOut < 0.01) return 1.0;
  if (vOut > SENSOR_VCC - 0.01) vOut = SENSOR_VCC - 0.01;
  float rs = rl * ((SENSOR_VCC - vOut) / vOut);
  Serial.printf("MQ135 AO=%.2f V  Rs=%.0f\n", vOut, rs);
  float r = rs / ro;

  static float smooth = -1;
  smooth = (smooth < 0) ? r : 0.9 * smooth + 0.1 * r;
  return smooth;
}

void readPMS5003() {
  Serial1.setTimeout(50);
  while (Serial1.available()) {
    if (Serial1.read() != 0x42) continue;
    if (Serial1.read() != 0x4D) continue;

    uint8_t buffer[30];
    if (Serial1.readBytes(buffer, 30) != 30) return;

    uint16_t sum = 0x42 + 0x4D;
    for (int i = 0; i < 28; i++) sum += buffer[i];
    uint16_t checksum = (buffer[28] << 8) | buffer[29];
    if (sum != checksum) continue;

    pm25 = (buffer[10] << 8) | buffer[11];
    pm10 = (buffer[12] << 8) | buffer[13];

    Serial.printf("PM2.5=%u PM10=%u\n", (unsigned)pm25, (unsigned)pm10);
  }
}

void handleRoot() {
  pixels.setPixelColor(0, pixels.Color(0, 0, 255));
  pixels.show();

  String html = "<!DOCTYPE html><html>";
  html += "<head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Live IoT Weather Station</title>";
  html += "<style>";
  html += "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: #0f172a; color: #f8fafc; margin: 0; padding: 20px; }";
  html += ".container { max-width: 600px; margin: auto; background: #1e293b; padding: 24px; border-radius: 16px; box-shadow: 0 10px 25px rgba(0,0,0,0.3); }";
  html += "h1 { text-align: center; color: #38bdf8; font-size: 24px; margin-bottom: 24px; }";
  html += ".grid { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }";
  html += ".card { background: #334155; padding: 16px; border-radius: 12px; border-left: 5px solid #38bdf8; transition: all 0.3s ease; }";
  html += ".card.orange { border-left-color: #f97316; }";
  html += ".card.green { border-left-color: #22c55e; }";
  html += ".card.purple { border-left-color: #a855f7; }";
  html += ".card.pink { border-left-color: #ec4899; }";
  html += ".card.teal { border-left-color: #14b8a6; }";
  html += ".card.alert { border-left-color: #ef4444; background: #451a03; }";
  html += ".title { font-size: 13px; color: #94a3b8; text-transform: uppercase; letter-spacing: 0.5px; }";
  html += ".value { font-size: 22px; font-weight: bold; margin-top: 8px; color: #f1f5f9; }";
  html += ".footer { text-align: center; margin-top: 20px; font-size: 12px; color: #64748b; }";
  html += "#alert-banner { display: none; background: #ef4444; color: white; padding: 10px; border-radius: 8px; text-align: center; font-weight: bold; margin-bottom: 16px; animation: pulse 1s infinite; }";
  html += "@keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.6; } 100% { opacity: 1; } }";
  html += "</style>";
  html += "<script>";

  html += "let audioCtx = null;";
  html += "function playBeep() {";
  html += "  if (!audioCtx) { audioCtx = new (window.AudioContext || window.webkitAudioContext)(); }";
  html += "  let osc = audioCtx.createOscillator();";
  html += "  let gain = audioCtx.createGain();";
  html += "  osc.type = 'sine';";
  html += "  osc.frequency.setValueAtTime(880, audioCtx.currentTime);";
  html += "  gain.gain.setValueAtTime(0.1, audioCtx.currentTime);";
  html += "  osc.connect(gain);";
  html += "  gain.connect(audioCtx.destination);";
  html += "  osc.start();";
  html += "  osc.stop(audioCtx.currentTime + 0.3);";
  html += "}";

  html += "function updateData() {";
  html += "  fetch('/data').then(response => response.json()).then(data => {";
  html += "    document.getElementById('temp').innerHTML = data.tempC.toFixed(1) + ' &deg;C / ' + data.tempF.toFixed(1) + ' &deg;F';";
  html += "    document.getElementById('humidity').innerHTML = data.humidity.toFixed(1) + ' %';";
  html += "    document.getElementById('pressure').innerHTML = data.pressure.toFixed(1) + ' hPa';";
  html += "    document.getElementById('mq135').innerHTML = data.ratio.toFixed(2) + ' <span style=\"font-size:14px;color:#94a3b8;\">Ratio</span>';";
  html += "    document.getElementById('pm25').innerHTML = data.pm25.toFixed(1) + ' &mu;g/m&sup3;';";
  html += "    document.getElementById('pm10').innerHTML = data.pm10.toFixed(1) + ' &mu;g/m&sup3;';";

  html += "    let tempCard = document.getElementById('temp-card');";
  html += "    let humCard = document.getElementById('hum-card');";
  html += "    let pressCard = document.getElementById('press-card');";
  html += "    let mqCard = document.getElementById('mq-card');";
  html += "    let pm25Card = document.getElementById('pm25-card');";
  html += "    let pm10Card = document.getElementById('pm10-card');";
  html += "    let banner = document.getElementById('alert-banner');";

  html += "    let alerts = [];";

  html += "    if (data.tempC > 45 || data.tempC < 0) { tempCard.className = 'card alert'; alerts.push('Temperature'); } else { tempCard.className = 'card'; }";
  html += "    if (data.humidity > 85 || data.humidity < 20) { humCard.className = 'card orange alert'; alerts.push('Humidity'); } else { humCard.className = 'card orange'; }";
  html += "    if (data.pressure < 550 || data.pressure > 1400) { pressCard.className = 'card purple alert'; alerts.push('Pressure'); } else { pressCard.className = 'card purple'; }";
  html += "    if (data.ratio < 0.60) { mqCard.className = 'card alert'; alerts.push('Air Quality'); } else { mqCard.className = 'card green'; }";
  html += "    if (data.pm25 > 65) { pm25Card.className = 'card alert'; alerts.push('PM2.5'); } else { pm25Card.className = 'card pink'; }";
  html += "    if (data.pm10 > 150.0) { pm10Card.className = 'card alert'; alerts.push('PM10'); } else { pm10Card.className = 'card teal'; }";

  html += "    if (alerts.length > 0) {";
  html += "      banner.style.display = 'block';";
  html += "      banner.innerHTML = 'WARNING: Abnormal readings in: ' + alerts.join(', ');";
  html += "      playBeep();";
  html += "    } else { banner.style.display = 'none'; }";

  html += "  }).catch(err => console.log('Error fetching data:', err));";
  html += "}";
  html += "setInterval(updateData, 2000);";
  html += "</script>";
  html += "</head><body>";

  html += "<div class='container'>";
  html += "<h1>Live IoT Weather Station</h1>";
  html += "<div id='alert-banner'></div>";
  html += "<div class='grid'>";

  html += "<div class='card' id='temp-card'><div class='title'>Temperature</div><div class='value' id='temp'>Loading...</div></div>";
  html += "<div class='card orange' id='hum-card'><div class='title'>Humidity</div><div class='value' id='humidity'>Loading...</div></div>";
  html += "<div class='card purple' id='press-card'><div class='title'>Pressure</div><div class='value' id='pressure'>Loading...</div></div>";
  html += "<div class='card green' id='mq-card'><div class='title'>Air Quality (Rs/Ro)</div><div class='value' id='mq135'>Loading...</div></div>";
  html += "<div class='card pink' id='pm25-card'><div class='title'>PM2.5 Dust</div><div class='value' id='pm25'>Loading...</div></div>";
  html += "<div class='card teal' id='pm10-card'><div class='title'>PM10 Dust</div><div class='value' id='pm10'>Loading...</div></div>";

  html += "</div>";
  html += "<div class='footer'>Live AJAX Stream &bull; Updates seamlessly every 2s</div>";
  html += "</div>";
  html += "</body></html>";

  server.send(200, "text/html; charset=utf-8", html);

  pixels.setPixelColor(0, pixels.Color(0, 250, 0));
  pixels.show();
}

void handleData() {
  float humidity = dht.readHumidity();
  float tempC = dht.readTemperature();
  float tempF = dht.readTemperature(true);

  float pressure = 728.0;
  float rawPressure = bmp.readPressure();
  if (!isnan(rawPressure) && rawPressure > 0) {
    pressure = rawPressure / 100.0F;
  }

  float ratio = readMq135Ratio();

  if (isnan(humidity)) humidity = 0;
  if (isnan(tempC)) { tempC = 0; tempF = 0; }

  String json = "{";
  json += "\"tempC\":" + String(tempC) + ",";
  json += "\"tempF\":" + String(tempF) + ",";
  json += "\"humidity\":" + String(humidity) + ",";
  json += "\"pressure\":" + String(pressure) + ",";
  json += "\"ratio\":" + String(ratio) + ",";
  json += "\"pm25\":" + String(pm25) + ",";
  json += "\"pm10\":" + String(pm10);
  json += "}";

  server.send(200, "application/json", json);
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8N1, PMS_RX, PMS_TX);
  delay(1000);

  pixels.begin();
  pixels.setBrightness(30);

  pinMode(mq135Pin, INPUT);
  analogSetAttenuation(ADC_11db);

  dht.begin();
  Wire.begin(8,9);

  if (!bmp.begin(0x76)) {
    if (!bmp.begin(0x77)) {
      Serial.println("Could not find a valid BMP280 sensor on 0x76 or 0x77!");
    } else {
      Serial.println("BMP280 found on secondary address 0x77!");
    }
  } else {
    Serial.println("BMP280 found on primary address 0x76!");
  }

  pixels.setPixelColor(0, pixels.Color(255, 0, 0));
  pixels.show();

  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempts++;
    if (attempts > 40) {
      Serial.println("\nWi-Fi connection failed!");
      pixels.setPixelColor(0, pixels.Color(128, 0, 128));
      pixels.show();
      break;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected! IP Address: http://");
    Serial.println(WiFi.localIP());

    pixels.setPixelColor(0, pixels.Color(0, 250, 0));
    pixels.show();
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
}

void loop() {
  server.handleClient();
  readPMS5003();   // keep the dust sensor serial buffer drained and values fresh
}
