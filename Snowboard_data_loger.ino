#include <TinyGPS++.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

// Objectes
TinyGPSPlus gps;
HardwareSerial gpsSerial(2); 
Adafruit_BME280 bme; 

// Config WiFi
const char* WIFI_SSID = "EV-OnWifi.cat"; //casa
//const char* WIFI_SSID = "vivo Y72 5G"; 
const char* WIFI_PASS = "onwifimola"; 
const char* SERVER_URL = "https://snowboard-api.onrender.com/upload";

// Pins i estats
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
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) { delay(500); }
}

void setup() {
  Serial.begin(115200);
  pinMode(BOTO_BOOT, INPUT_PULLUP);
  pinMode(PIN_LED_ESTAT, OUTPUT);
  
  Wire.begin(); 
  if (!bme.begin(0x76)) Serial.println("Error BME280");
  
  gpsSerial.begin(9600, SERIAL_8N1, 16, -1);
  connectaWiFi();
  
  // Generem codi de vinculació aleatori
  pair_code = String(esp_random() % 1000000);
  while(pair_code.length() < 6) pair_code = "0" + pair_code;
}

void loop() {
  // Llegir GPS constantment
  while (gpsSerial.available()) { gps.encode(gpsSerial.read()); }

  unsigned long currentMillis = millis();

  // Lògica LED
  if (gravant) {
    if (currentMillis - lastBlinkTime >= blinkInterval) {
      lastBlinkTime = currentMillis;
      ledState = !ledState;
      digitalWrite(PIN_LED_ESTAT, ledState);
    }
  } else {
    digitalWrite(PIN_LED_ESTAT, isLinked ? HIGH : LOW);
  }

  // Lògica Botó (Toggle Gravació)
  int estatBoto = digitalRead(BOTO_BOOT);
  if (ultimEstatBoto == HIGH && estatBoto == LOW) {
    delay(50);
    if (digitalRead(BOTO_BOOT) == LOW) gravant = !gravant;
  }
  ultimEstatBoto = estatBoto;

  // Enviament cada 5 segons
  if (currentMillis - lastSendMs > 5000) {
    lastSendMs = currentMillis;
    connectaWiFi();

    if (WiFi.status() == WL_CONNECTED) {
      String json = "{";
      json += "\"device_id\":\"" + WiFi.macAddress() + "\",";
      json += "\"pair_code\":\"" + pair_code + "\",";
      json += "\"lat\":" + String(gps.location.isValid() ? gps.location.lat() : 0.0, 6) + ",";
      json += "\"lon\":" + String(gps.location.isValid() ? gps.location.lng() : 0.0, 6) + ",";
      json += "\"alt\":" + String(gps.altitude.isValid() ? gps.altitude.meters() : 0.0, 2) + ",";
      json += "\"spd\":" + String(gps.speed.isValid() ? gps.speed.kmph() : 0.0, 2) + ",";
      json += "\"course\":" + String(gps.course.isValid() ? gps.course.deg() : -1.0, 2) + ",";
      json += "\"gravant\":" + String(gravant ? "true" : "false") + ",";
      json += "\"temp\":" + String(bme.readTemperature(), 2) + ",";
      json += "\"hum\":" + String(bme.readHumidity(), 2) + ",";
      json += "\"pres\":" + String(bme.readPressure() / 100.0F, 2);
      json += "}";

      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient http;
      http.begin(client, SERVER_URL);
      http.addHeader("Content-Type", "application/json");
      
      int code = http.POST(json);
      if (code > 0) {
        String res = http.getString();
        isLinked = (res.indexOf("\"status\":\"linked\"") != -1);
      }
      http.end();
    }
  }
}