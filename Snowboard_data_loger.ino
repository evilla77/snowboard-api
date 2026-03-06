#include <TinyGPS++.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

// ---------------- GPS ----------------
TinyGPSPlus gps;
HardwareSerial gpsSerial(2); 

// ---------------- SENSOR BME280 ----------------
Adafruit_BME280 bme; 

// ---------------- Config ----------------
#define DEBUG_HUMA 1  

// Variables per connectar al wifi
//const char* WIFI_SSID = "EV-OnWifi.cat"; //casa
const char* WIFI_SSID = "vivo Y72 5G"; // mòvil
const char* WIFI_PASS = "onwifimola"; //casa

const int BOTO_BOOT = 0;       
bool gravant = false;          
const int PIN_LED_ESTAT = 2;  
int ultimEstatBoto = HIGH;

bool isLinked = false;
unsigned long lastBlinkTime = 0;
const int blinkInterval = 500;
bool ledState = LOW;

String pair_code = "";
const char* SERVER_URL = "https://snowboard-api.onrender.com/upload";
unsigned long lastSendMs = 0;

// ---------------- Utilitats ----------------

const char* direccio(double graus) {
  static const char* dirs[] = {"N","NE","E","SE","S","SO","O","NO"};
  int i = (int)((graus + 22.5) / 45.0);
  return dirs[i % 8];
}

void print2(int v) {
  if (v < 10) Serial.print('0');
  Serial.print(v);
}

void llegeixGPS() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }
}

void connectaWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connectant a WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK. IP: ");
    Serial.println(WiFi.localIP());
  }
}

void mantinguesWiFi() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 5000) return;
  lastCheck = millis();
  if (WiFi.status() != WL_CONNECTED) connectaWiFi();
}

// Imprimeix dades “per humans” (ACTUALITZAT AMB PRESSIÓ)
void printDebug(double lat, double lon, double alt, double spd, double courseDeg, const char* courseTxt,
                int h, int m, int s, float temp, float hum, float pres) {
  if (!DEBUG_HUMA) return;

  Serial.println("\n---- STATUS DISPOSITIU ----");
  Serial.print("Temp: "); Serial.print(temp); Serial.print(" *C | ");
  Serial.print("Hum: "); Serial.print(hum); Serial.print(" % | ");
  Serial.print("Pres: "); Serial.print(pres); Serial.println(" hPa");
  
  Serial.println("---- GPS ----");
  Serial.print("Lat: "); Serial.println(lat, 6);
  Serial.print("Lon: "); Serial.println(lon, 6);
  Serial.print("Sats: "); Serial.println(gps.satellites.value());
  Serial.print("Altitud GPS (m): "); Serial.println(alt, 2);
  Serial.print("Velocitat (km/h): "); Serial.println(spd, 2);

  if (courseDeg >= 0.0) {
    Serial.print("Rumb: "); Serial.print(courseDeg, 1);
    Serial.print("° ("); Serial.print(courseTxt); Serial.println(")");
  } else {
    Serial.println("Rumb: -- (parat)");
  }

  Serial.print("Hora Espanya: ");
  if (h >= 0) {
    print2(h); Serial.print(":");
    print2(m); Serial.print(":");
    print2(s);
  } else {
    Serial.print("--:--:--");
  }
  Serial.println("\n---------------------------\n");
}

String construeixJSON(double lat, double lon, double alt, double spd, double courseDeg,
                      const char* courseTxt, int h, int m, int s, bool rec, 
                      float temp, float hum, float pres) {

  String dispositiu_id = WiFi.macAddress();
  String json = "{";
  json += "\"t_ms\":" + String(millis());
  json += ",\"device_id\":\"" + dispositiu_id + "\"";
  json += ",\"pair_code\":\"" + pair_code + "\"";
  json += ",\"lat\":" + String(lat, 6);
  json += ",\"lon\":" + String(lon, 6);
  json += ",\"alt_m\":" + String(alt, 2);
  json += ",\"spd_kmh\":" + String(spd, 2);
  json += ",\"course_deg\":" + String(courseDeg, 1);
  json += ",\"course_txt\":\"" + String(courseTxt) + "\"";
  json += ",\"hour\":" + String(h);
  json += ",\"min\":" + String(m);
  json += ",\"sec\":" + String(s);
  json += ",\"gravant\":" + String(rec ? "true" : "false");
  json += ",\"temp\":" + String(temp, 2);
  json += ",\"hum\":" + String(hum, 2);
  json += ",\"pres\":" + String(pres, 2); // <--- AFEGIT AL JSON
  json += "}";
  return json;
}

void enviaHTTP(const String& json) {
  if (WiFi.status() != WL_CONNECTED) return;
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  
  int code = http.POST((uint8_t*)json.c_str(), json.length());
  
  if (code > 0) {
    String response = http.getString();
    if (response.indexOf("\"status\":\"linked\"") != -1) isLinked = true;
    else isLinked = false;
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(BOTO_BOOT, INPUT_PULLUP);
  pinMode(PIN_LED_ESTAT, OUTPUT);

  // Inicialitzar BME280
  Wire.begin(); 
  if (!bme.begin(0x76)) {
    Serial.println("ALERTA: BME280 no trobat!");
  }

  gpsSerial.begin(9600, SERIAL_8N1, 16, -1);
  connectaWiFi();
  pair_code = String(esp_random() % 1000000).substring(0,6);
}

void loop() {
  llegeixGPS();

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

  mantinguesWiFi();

  if (millis() - lastSendMs < 5000) return;
  lastSendMs = millis();

  // Lectura sensors BME
  float t = bme.readTemperature();
  float h = bme.readHumidity();
  float p_hpa = bme.readPressure() / 100.0F; // Convertim a hPa

  // Dades GPS
  double lat = gps.location.isValid() ? gps.location.lat() : 0.0;
  double lon = gps.location.isValid() ? gps.location.lng() : 0.0;
  double alt = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
  double spd = gps.speed.isValid() ? gps.speed.kmph() : 0.0;
  
  double courseDeg = (spd >= 1.0 && gps.course.isValid()) ? gps.course.deg() : -1.0;
  const char* courseTxt = (courseDeg >= 0) ? direccio(courseDeg) : "--";

  int hr = -1, mn = -1, sc = -1;
  if (gps.time.isValid()) {
    hr = gps.time.hour() + 1; if (hr >= 24) hr -= 24;
    mn = gps.time.minute();
    sc = gps.time.second();
  }

  // --- PRIMER: IMPRIMIR DEBUG PER HUMANS (AMB TEMP/HUM/PRES) ---
  printDebug(lat, lon, alt, spd, courseDeg, courseTxt, hr, mn, sc, t, h, p_hpa);

  // --- SEGON: CONSTRUIR I ENVIAR JSON ---
  String jsonStr = construeixJSON(lat, lon, alt, spd, courseDeg, courseTxt, hr, mn, sc, gravant, t, h, p_hpa);
  
  if (DEBUG_HUMA) {
    Serial.print(">>> JSON ENVIAT: ");
    Serial.println(jsonStr);
  }
  
  enviaHTTP(jsonStr);
}