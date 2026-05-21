#include <TinyGPS++.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include "esp_sleep.h" // Llibreria per al Light Sleep (Gestió d'energia)

// MODIFICACIÓ: Llibreries per a la pantalla OLED (Eliminada la LiquidCrystal_I2C)
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128 // Amplada de l'OLED en píxels
#define SCREEN_HEIGHT 64 // Alçada de l'OLED en píxels

// Inicialitzem la pantalla OLED a l'adreça 0x3C trobada per l'escàner
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

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
unsigned long lastSendMs = 0;
String pair_code = "";

void connectaWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) { delay(500); Serial.print("."); }
}

// FUNCIO DEBUG RECUPERADA (No es filtra res)
void printDebugLocal(float t, float h, float p) {
  Serial.println("\n--- DADES ACTUALS ---");
  Serial.printf("TEMP: %.2f C | HUM: %.2f %% | PRES: %.2f hPa\n", t, h, p);
  Serial.printf("GPS: Lat %.6f, Lon %.6f | Sats: %d\n", gps.location.lat(), gps.location.lng(), gps.satellites.value());
  Serial.printf("ESTAT: %s\n", gravant ? "GRAVANT" : "ESPERANT");
  Serial.println("---------------------\n");
}

void setup() {
  // MODIFICACIÓ CRÍTICA: Baixem a 80MHz al principi de tot perquè afecti correctament els càlculs de velocitat dels ports sèrie anteriors
  setCpuFrequencyMhz(80);

  Serial.begin(115200);
  delay(1000); // Temps segur per obrir el port sèrie a Windows

  // IMPRIMIR LA MAC PER PORT SÈRIE (Solicitat anteriorment)
  Serial.println("\n=================================");
  Serial.print("La meva MAC es: ");
  Serial.println(WiFi.macAddress());
  Serial.println("=================================\n");

  pinMode(BOTO_BOOT, INPUT_PULLUP);
  pinMode(PIN_LED_ESTAT, OUTPUT);
  
  // INICI DEL BUS I2C
  Wire.begin(); 
  Wire.setClock(100000); // Velocitat estable per evitar soroll amb el sensor de temp
  
  bme.begin(0x76);
  gpsSerial.begin(9600, SERIAL_8N1, 16, -1);
  connectaWiFi();
  pair_code = String(esp_random() % 1000000);
  while(pair_code.length() < 6) pair_code = "0" + pair_code;

  // COMENTARI NOU: Configurem que el botó pugui despertar l'ESP32 del Light Sleep instantàniament
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); 

  // MODIFICACIÓ: Engeguem la pantalla OLED amb el missatge "hola maduixa"
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println(F("Error: No s'ha pogut iniciar l'OLED"));
  } else {
    display.clearDisplay();      // Neteja la memòria de la pantalla
    display.setTextSize(2);      // Mida de lletra gran
    display.setTextColor(SSD1306_WHITE); // Color blanc
    display.setCursor(10, 20);   // Posició a la pantalla (X, Y)
    display.print("hola\n  maduixa"); 
    display.display();           // Envia les dades físicament a la pantalla per pintar-les
  }
}

void loop() {
  // 1. LLEGIR GPS
  while (gpsSerial.available()) { gps.encode(gpsSerial.read()); }
  unsigned long currentMillis = millis();

  // 2. LECTURA DEL BOTÓ (Commuta entre GRAVANT i ESPERANT)
  int estatBoto = digitalRead(BOTO_BOOT);
  if (ultimEstatBoto == HIGH && estatBoto == LOW) {
    delay(50);
    if (digitalRead(BOTO_BOOT) == LOW) gravant = !gravant;
  }
  ultimEstatBoto = estatBoto;

  // 3. ENVIAMENT DE DADES (CADA 5 SEGONS)
  if (currentMillis - lastSendMs > 5000) {
    lastSendMs = currentMillis;
    
    float t = bme.readTemperature();
    float h = bme.readHumidity();
    float p = bme.readPressure() / 100.0F;

    printDebugLocal(t, h, p); // DISPLAY LOCAL (Comentari recuperat)

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
      json += "\"temp\":" + (isnan(t) ? "null" : String(t, 2)) + ",";
      json += "\"hum\":" + (isnan(h) ? "null" : String(h, 2)) + ",";
      json += "\"pres\":" + (isnan(p) ? "null" : String(p, 2));
      json += "}";

      // IMPRIMIR JSON PER PANTALLA (Petició de l'usuari)
      Serial.println("--- JSON ENVIAT ---");
      Serial.println(json);
      Serial.println("-------------------\n");

      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient http;
      http.begin(client, SERVER_URL);
      http.addHeader("Content-Type", "application/json");
      
      int code = http.POST(json);
      
      if (code > 0) {
        isLinked = (http.getString().indexOf("\"status\":\"linked\"") != -1);
      }
      http.end();
    }
  }
}