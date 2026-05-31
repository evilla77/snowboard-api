#include <TinyGPS++.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include "esp_sleep.h" 

#include "MPU6050.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// LLIBRIERIES DEL PORTAL DE CONFIGURACIÓ
#include <DNSServer.h> 
#include <WebServer.h> 
#include <Preferences.h> 

#define SCREEN_WIDTH 128 
#define SCREEN_HEIGHT 64 

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

TinyGPSPlus gps;
HardwareSerial gpsSerial(2); 
Adafruit_BME280 bme; 
MPU6050 mpu;

Preferences preferences; 
DNSServer dnsServer; 
WebServer server(80); 

String wifi_ssid = "";
String wifi_pass = ""; 
const char* AP_SSID = "WhiteBoX-CONFIG"; 
bool portalActiu = false;               

const char* SERVER_URL = "https://snowboard-api.onrender.com/upload";

const int BOTO_BOOT = 14;       
const int PIN_LED_ESTAT = 2;  
volatile bool gravant = false;          
bool isLinked = false;
int ultimEstatBoto = HIGH;
unsigned long lastSendMs = 0;
String pair_code = "";

unsigned long lastPairCodeRenewMs = 0; 
const unsigned long RENEW_PAIR_CODE_INTERVAL = 300000; 

// Control d'interrupció i temps del Portal AP (4 minuts màxim)
volatile unsigned long ultimTempsInterrupcio = 0;
volatile bool demanaPortalAP = false; 
unsigned long tempsIniciPortal = 0;
const unsigned long TEMPS_MAX_PORTAL = 240000; // 4 minuts en mil·lisegons

// =======================================================================
// INTERRUPCIÓ CORREGIDA: SENSE TEMPORITZADORS DE CONTROL
// =======================================================================
void IRAM_ATTR manejadorBoto() {
  unsigned long tempsActual = millis();
  
  // Filtre físic anti-rebots bàsic de 300ms
  if (tempsActual - ultimTempsInterrupcio > 300) {
    
    // CAS A: Si està connectat, un clic canvia el mode de gravació (la teva lògica)
    if (!portalActiu && WiFi.status() == WL_CONNECTED && isLinked) {
      gravant = !gravant; 
    }
    // CAS B: Si està offline, un clic normal demana obrir el portal
    else if (!portalActiu && WiFi.status() != WL_CONNECTED) {
      demanaPortalAP = true; 
    }
    
    ultimTempsInterrupcio = tempsActual;
  }
}

// VISTES HTML DEL PORTAL
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1' charset='UTF-8'>";
  html += "<style>body{font-family:Arial; text-align:center; background:#121212; color:white; padding:20px;}";
  html += "input[type=text], input[type=password]{width:80%; padding:12px; margin:10px 0; border:none; border-radius:4px;}";
  html += "input[type=submit]{background:#00adb5; color:white; padding:14px 20px; border:none; border-radius:4px; cursor:pointer; width:84%; font-size:16px;}";
  html += "</style></head><body>";
  html += "<h2>Rider Tracker Config</h2>";
  html += "<form action='/save' method='POST'>";
  html += "<input type='text' name='ssid' placeholder='Nom del Wi-Fi (SSID)' required><br>";
  html += "<input type='password' name='pass' placeholder='Contrasenya Wi-Fi'><br><br>";
  html += "<input type='submit' value='Guardar i Connectar'>";
  html += "</form></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("ssid")) {
    wifi_ssid = server.arg("ssid");
    wifi_pass = server.arg("pass");
    
    preferences.begin("wifi-config", false);
    preferences.putString("ssid", wifi_ssid);
    preferences.putString("pass", wifi_pass);
    preferences.end();
    
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'></head><body style='background:#121212; color:white; font-family:Arial; text-align:center; padding-top:50px;'>";
    html += "<h3>Configuració desada amb èxit!</h3><p>L'ESP32 es reiniciarà per connectar-se a: <b>" + wifi_ssid + "</b></p></body></html>";
    server.send(200, "text/html", html);
    
    delay(3000); 
    dnsServer.stop();
    server.stop();
    ESP.restart(); 
  }
}

void printDebugLocal(float t, float h, float p) {
  Serial.println("\n--- DADES ACTUALS ---");
  Serial.printf("TEMP: %.2f C | HUM: %.2f %% | PRES: %.2f hPa\n", t, h, p);
  Serial.printf("GPS: Lat %.6f, Lon %.6f | Sats: %d\n", gps.location.lat(), gps.location.lng(), gps.satellites.value());
  Serial.printf("ESTAT: %s\n", gravant ? "GRAVANT" : "ESPERANT");
  Serial.println("---------------------\n");
}

void generaNouPairCode() {
  pair_code = String(esp_random() % 1000000);
  while(pair_code.length() < 6) pair_code = "0" + pair_code;
  Serial.print("--- NOU PAIR CODE GENERAT: ");
  Serial.println(pair_code);
}

void setup() {
  setCpuFrequencyMhz(80);
  Serial.begin(115200);
  delay(1000); 

  pinMode(BOTO_BOOT, INPUT_PULLUP);
  pinMode(PIN_LED_ESTAT, OUTPUT);
  
  attachInterrupt(digitalPinToInterrupt(BOTO_BOOT), manejadorBoto, FALLING);

  Wire.begin(); 
  Wire.setClock(100000); 
  
  mpu.initialize();
  mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_16);
  mpu.setFullScaleGyroRange(MPU6050_GYRO_FS_2000);

  bme.begin(0x76);
  gpsSerial.begin(9600, SERIAL_8N1, 16, -1);

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println(F("Error OLED")); 
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    
    // Nom del dispositiu ben gran
    display.setTextSize(2);
    display.setCursor(15, 20);
    display.print("WhiteBoX"); 
    
    // Subtítol de sistema en petit
    display.setTextSize(1);
    display.setCursor(9, 45);
    display.print("SNOWBOARD TELEMETRY"); 
    
    display.display();
  }
  
  generaNouPairCode();
  lastPairCodeRenewMs = millis(); 

  // Llegir de la memòria
  preferences.begin("wifi-config", true);
  wifi_ssid = preferences.getString("ssid", ""); 
  wifi_pass = preferences.getString("pass", "");
  preferences.end();

  if (wifi_ssid != "") {
    Serial.print("Intentant connectar al Wi-Fi desat: ");
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) { 
      delay(500); 
      Serial.print("."); 
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_OFF);
    portalActiu = false;
    Serial.println("\nSense Wi-Fi disponible. Mode Offline actiu.");
  } else {
    portalActiu = false;
  }
  
  display.clearDisplay();
  display.display();
  ultimEstatBoto = digitalRead(BOTO_BOOT);
  
  // Ignorem qualsevol micro-caiguda de tensió de l'arrencada posant a fals la petició
  demanaPortalAP = false; 
}

void loop() {
  unsigned long currentMillis = millis();

  // =======================================================================
  // ACCIÓ DEL BOTÓ RECIBIDA PER INTERRUPCIÓ (CLIC SIMPLE SENSE COMPLICACIONS)
  // =======================================================================
  if (demanaPortalAP && !portalActiu) {
    demanaPortalAP = false; // Resetejem l'ordre
    
    Serial.println("-> Clic detectat correctament. Obrint Portal per 4 minuts...");
    portalActiu = true;
    tempsIniciPortal = currentMillis; // Guardem l'hora d'obertura pel timeout
    
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID); 
    dnsServer.start(53, "*", WiFi.softAPIP()); 
    server.on("/", handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound(handleRoot); 
    server.begin();
  }

  // =======================================================================
  // TIMEOUT DE TANCAMENT: 4 MINUTS D'AUTO-APAGAT
  // =======================================================================
  if (portalActiu) {
    if (currentMillis - tempsIniciPortal >= TEMPS_MAX_PORTAL) {
      Serial.println("-> Han passat 4 minuts. Tancant portal per seguretat/energia...");
      dnsServer.stop();
      server.stop();
      WiFi.mode(WIFI_OFF); 
      portalActiu = false;
      return;
    }

    dnsServer.processNextRequest();
    server.handleClient(); 
    
    static unsigned long lastOledPortalMs = 0;
    if (currentMillis - lastOledPortalMs > 200) {
      lastOledPortalMs = currentMillis;
      
      unsigned long segonsRestants = (TEMPS_MAX_PORTAL - (currentMillis - tempsIniciPortal)) / 1000;

      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1); display.setCursor(5, 5); display.print("WhiteBoX CONFIG");
      display.setCursor(5, 22); display.print("SSID: WhiteBoX");
      display.setCursor(5, 42); display.print("Time limit auto-off:");
      display.setCursor(5, 54); display.print(String(segonsRestants) + " seconds left");
      display.display();
    }
    return; 
  }

  // =======================================================================
  // LA TEVA LÒGICA ORIGINAL DE LECTURA I ENVIAMENT MODIFICADA
  // =======================================================================
  while (gpsSerial.available()) { gps.encode(gpsSerial.read()); }

  if (wifi_ssid != "" && WiFi.status() != WL_CONNECTED && (currentMillis - lastSendMs > 15000)) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
  }

  // MODIFICACIÓ: Definim l'interval segons si està gravant o no
unsigned long intervalEnviament = (gravant || !isLinked) ? 5000 : 60000;// 5s si grava o pending, 1 minut (60000ms) si està en espera

  if (currentMillis - lastSendMs > intervalEnviament) {
    lastSendMs = currentMillis;
    
    if (!isLinked && (currentMillis - lastPairCodeRenewMs > RENEW_PAIR_CODE_INTERVAL)) {
      lastPairCodeRenewMs = currentMillis;
      generaNouPairCode(); 
    }

    float t = bme.readTemperature();
    float h = bme.readHumidity();
    float p = bme.readPressure() / 100.0F;

    int16_t rawAX, rawAY, rawAZ;
    int16_t rawGX, rawGY, rawGZ;
    mpu.getMotion6(&rawAX, &rawAY, &rawAZ, &rawGX, &rawGY, &rawGZ);

    printDebugLocal(t, h, p); 

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

      Serial.println("--- JSON ENVIAT ---");
      Serial.println(json);
      Serial.println("-------------------\n");

      WiFiClientSecure client; client.setInsecure(); HTTPClient http;
      http.begin(client, SERVER_URL); http.addHeader("Content-Type", "application/json");
      
      int code = http.POST(json);
      
      if (code > 0) {
        String respostaServidor = http.getString();
        Serial.print("--- RESP_SERVIDOR: ");
        Serial.println(respostaServidor);

        if (respostaServidor.indexOf("linked") != -1) {
          isLinked = true;
        } else {
          isLinked = false;
        }
      } else {
        Serial.printf("Error HTTP de connexio: %d\n", code);
      }
      http.end();
    }
  }

  // REFRESC ASÍNCRON DE LES TEVES PANTALLES ORDINÀRIES (Cada 200ms)
  static unsigned long lastOledRefreshMs = 0;
  if (millis() - lastOledRefreshMs > 200) {
    lastOledRefreshMs = millis();

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    
    if (WiFi.status() != WL_CONNECTED) {
      display.setTextSize(2); display.setCursor(15, 28); display.print("NO-WIFI"); 
    } 
    else {
      if (isLinked) {
        display.setTextSize(2); display.setCursor(0, 20);
        if (gravant) display.print("RECORDING"); else display.print("linked");    
      } else {
        display.setTextSize(2); display.setCursor(15, 20); display.print("pending");
        display.setTextSize(2); display.setCursor(5, 45); 
        display.print("PC:"); display.print(pair_code);
      }
    }
    display.display(); 
  }
}