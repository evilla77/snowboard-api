#include <TinyGPS++.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

TinyGPSPlus gps;
HardwareSerial gpsSerial(2); 
Adafruit_BME280 bme; 

const char* WIFI_SSID = "EV-OnWifi.cat"; 
//const char* WIFI_SSID = "vivo Y72 5G";
const char* WIFI_PASS = "onwifimola"; 
const char* SERVER_URL = "https://snowboard-api.onrender.com/upload";

const int BOTO_BOOT = 0;       
const int PIN_LED_ESTAT = 2;  
bool gravant = false;          
bool isLinked = false;
int ultimEstatBoto = HIGH;
unsigned long lastBlinkTime = 0;
unsigned long lastSendMs = 0;
const int blinkInterval = 500;
bool ledState = LOW;
String pair_code = "";

void connectaWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) { delay(500); Serial.print("."); }
}

// FUNCIO DEBUG RECUPERADA
void printDebugLocal(float t, float h, float p) {
  Serial.println("\n--- DADES ACTUALS ---");
  Serial.printf("TEMP: %.2f C | HUM: %.2f %% | PRES: %.2f hPa\n", t, h, p);
  Serial.printf("GPS: Lat %.6f, Lon %.6f | Sats: %d\n", gps.location.lat(), gps.location.lng(), gps.satellites.value());
  Serial.printf("ESTAT: %s\n", gravant ? "GRAVANT" : "ESPERANT");
  Serial.println("---------------------\n");
}

void setup() {
  Serial.begin(115200);
  pinMode(BOTO_BOOT, INPUT_PULLUP);
  pinMode(PIN_LED_ESTAT, OUTPUT);
  Wire.begin(); 
  bme.begin(0x76);
  gpsSerial.begin(9600, SERIAL_8N1, 16, -1);
  connectaWiFi();
  pair_code = String(esp_random() % 1000000);
  while(pair_code.length() < 6) pair_code = "0" + pair_code;
}

void loop() {
  while (gpsSerial.available()) { gps.encode(gpsSerial.read()); }
  unsigned long currentMillis = millis();

  if (gravant) {
    if (currentMillis - lastBlinkTime >= blinkInterval) {
      lastBlinkTime = currentMillis;
      ledState = !ledState;
      digitalWrite(PIN_LED_ESTAT, ledState);
    }
  } else {
    digitalWrite(PIN_LED_ESTAT, isLinked ? HIGH : LOW);
  }

  int estatBoto = digitalRead(BOTO_BOOT);
  if (ultimEstatBoto == HIGH && estatBoto == LOW) {
    delay(50);
    if (digitalRead(BOTO_BOOT) == LOW) gravant = !gravant;
  }
  ultimEstatBoto = estatBoto;

  if (currentMillis - lastSendMs > 5000) {
    lastSendMs = currentMillis;
    
    float t = bme.readTemperature();
    float h = bme.readHumidity();
    float p = bme.readPressure() / 100.0F;

    printDebugLocal(t, h, p); // DISPLAY LOCAL

    if (WiFi.status() == WL_CONNECTED) {
      String json = "{";
      json += "\"device_id\":\"" + WiFi.macAddress() + "\",";
      json += "\"pair_code\":\"" + pair_code + "\",";
      json += "\"lat\":" + String(gps.location.lat(), 6) + ",";
      json += "\"lon\":" + String(gps.location.lng(), 6) + ",";
      json += "\"alt\":" + String(gps.altitude.meters(), 2) + ",";
      json += "\"spd\":" + String(gps.speed.kmph(), 2) + ",";
      json += "\"course\":" + String(gps.course.deg(), 2) + ",";
      json += "\"gravant\":" + String(gravant ? "true" : "false") + ",";
      json += "\"temp\":" + String(t, 2) + ",";
      json += "\"hum\":" + String(h, 2) + ",";
      json += "\"pres\":" + String(p, 2);
      json += "}";

      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient http;
      http.begin(client, SERVER_URL);
      http.addHeader("Content-Type", "application/json");
      int code = http.POST(json);
      if (code > 0) isLinked = (http.getString().indexOf("\"status\":\"linked\"") != -1);
      http.end();
    } else {
      connectaWiFi();
    }
  }
}