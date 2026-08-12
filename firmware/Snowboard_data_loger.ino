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
// VARIABLES DE TELEMETRIA DE SALTS I TRUCS
// =======================================================================
int jumpCount = 0;
unsigned long tempsIniciVol = 0;
bool volant = false;
float maxAirtimeSessio = 0.0;
float maxSpinSessio = 0.0;
float maxLandingG = 0.0;

// Comptadors de trucs segons la rotació realitzada
int count180 = 0;
int count360 = 0;
int count540 = 0;
int count720 = 0;
int countAeriNet = 0; // Salts rectes (Straight Airs)

// Variables per a l'acumulador del giroscopi
float grausGiratsActuals = 0.0;
unsigned long ultimTempsCalculMpu = 0;

// Variables per desar les lectures directes de l'IMU sense processar
float axLlegit = 0.0, ayLlegit = 0.0, azLlegit = 0.0;
float gxLlegit = 0.0, gyLlegit = 0.0, gzLlegit = 0.0;
float gTotalLlegit = 0.0;

// =======================================================================
// INTERRUPCIÓ OPTIMITZADA EN ENERGIA I SEGREURETAT DE TEMPS
// =======================================================================
void IRAM_ATTR manejadorBoto() {
  unsigned long tempsActual = millis();
  
  if (tempsActual - ultimTempsInterrupcio > 300) {
    if (!portalActiu) {
      if (WiFi.status() == WL_CONNECTED && isLinked) {
        gravant = !gravant; 
        lastSendMs = 0; 
        
        if (gravant) {
          ultimTempsCalculMpu = tempsActual; 
        }
        else {
          jumpCount = 0;
          countAeriNet = 0;
          count180 = 0;
          count360 = 0;
          count540 = 0;
          count720 = 0;
          maxAirtimeSessio = 0.0;
          maxSpinSessio = 0.0;
          maxLandingG = 0.0;
          volant = false;
          grausGiratsActuals = 0.0;
        }
      }
      else if (WiFi.status() == WL_CONNECTED && !isLinked) {
        gravant = false; 
      }
      else if (WiFi.status() != WL_CONNECTED) {
        gravant = false;
        demanaPortalAP = true; 
      }
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
  html += "<h2>WhiteBoX Config</h2>";
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
    
    display.setTextSize(2);
    display.setCursor(15, 20);
    display.print("WhiteBoX"); 
    
    display.setTextSize(1);
    display.setCursor(9, 45);
    display.print("SNOWBOARD TELEMETRY"); 
    
    display.display();
  }
  
  generaNouPairCode();
  lastPairCodeRenewMs = millis(); 

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
  
  demanaPortalAP = false; 
  ultimTempsCalculMpu = millis();
}

void loop() {
  unsigned long currentMillis = millis();

  // =======================================================================
  // 1. LECTURA DE L'IMU: NOMÉS SI ESTÀ LINKED I GRAVANT
  // =======================================================================
  if (isLinked && gravant && (currentMillis - ultimTempsCalculMpu >= 20)) {
    float dt = (currentMillis - ultimTempsCalculMpu) / 1000.0; 
    ultimTempsCalculMpu = currentMillis;

    int16_t rawAX, rawAY, rawAZ;
    int16_t rawGX, rawGY, rawGZ;
    mpu.getMotion6(&rawAX, &rawAY, &rawAZ, &rawGX, &rawGY, &rawGZ);

    float ax = (float)rawAX / 2048.0;
    float ay = (float)rawAY / 2048.0;
    float az = (float)rawAZ / 2048.0;
    float gz = (float)rawGZ / 16.4;

    // Desem les lectures directes
    axLlegit = ax;
    ayLlegit = ay;
    azLlegit = az;
    gxLlegit = (float)rawGX / 16.4;
    gyLlegit = (float)rawGY / 16.4;
    gzLlegit = gz;

    float gTotal = sqrt(ax*ax + ay*ay + az*az);
    gTotalLlegit = gTotal;

    if (!volant && gTotal < 0.4) {
      volant = true;
      tempsIniciVol = currentMillis;
      grausGiratsActuals = 0.0; 
    }

    if (volant) {
      grausGiratsActuals += abs(gz) * dt; 
    }

    if (volant && gTotal > 1.4) {
      volant = false;
      unsigned long duradaVolMs = currentMillis - tempsIniciVol;

      if (duradaVolMs > 150) {
        jumpCount++; 
        float tempsVolSegons = duradaVolMs / 1000.0;

        if (tempsVolSegons > maxAirtimeSessio) maxAirtimeSessio = tempsVolSegons;
        if (grausGiratsActuals > maxSpinSessio) maxSpinSessio = grausGiratsActuals;
        if (gTotal > maxLandingG) maxLandingG = gTotal;

        if (grausGiratsActuals >= 90 && grausGiratsActuals < 270) { count180++; } 
        else if (grausGiratsActuals >= 270 && grausGiratsActuals < 450) { count360++; } 
        else if (grausGiratsActuals >= 450 && grausGiratsActuals < 630) { count540++; } 
        else if (grausGiratsActuals >= 630) { count720++; }
        else if (grausGiratsActuals < 90) { countAeriNet++; }
      }
    }
  }

  // =======================================================================
  // 2. PORTAL DE CONFIGURACIÓ
  // =======================================================================
  if (demanaPortalAP && !portalActiu) {
    demanaPortalAP = false; 
    Serial.println("-> Clic detectat correctament. Obrint Portal per 4 minuts...");
    portalActiu = true;
    tempsIniciPortal = currentMillis; 
    
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID); 
    dnsServer.start(53, "*", WiFi.softAPIP()); 
    server.on("/", handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound(handleRoot); 
    server.begin();
  }

  if (portalActiu) {
    if (currentMillis - tempsIniciPortal >= TEMPS_MAX_PORTAL) {
      Serial.println("-> Han passat 4 minuts. Tancant portal...");
      dnsServer.stop(); server.stop(); WiFi.mode(WIFI_OFF); portalActiu = false;
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
  // 3. LECTURA DEL GPS: NOMÉS SI ESTÀ LINKED
  // =======================================================================
  if (isLinked) {
    while (gpsSerial.available()) { gps.encode(gpsSerial.read()); }
  }

  // Reconnexió WiFi automàtica
  if (wifi_ssid != "" && WiFi.status() != WL_CONNECTED && (currentMillis - lastSendMs > 15000)) {
    WiFi.mode(WIFI_STA); WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
  }

  // =======================================================================
  // 4. PREPARACIÓ I ENVIAMENT HTTP / LOGS SÈRIE
  // =======================================================================
  if (!isLinked) gravant = false;

  unsigned long intervalEnviament = (gravant || !isLinked) ? 5000 : 60000;

  if (currentMillis - lastSendMs > intervalEnviament) {
    lastSendMs = currentMillis;
    
    if (!isLinked && (currentMillis - lastPairCodeRenewMs > RENEW_PAIR_CODE_INTERVAL)) {
      lastPairCodeRenewMs = currentMillis; generaNouPairCode(); 
    }

    if (WiFi.status() == WL_CONNECTED) {
      
      float latVal = 0.0, lonVal = 0.0, altVal = 0.0, spdVal = 0.0, crsVal = 0.0;
      float tempVal = 0.0, humVal = 0.0, presVal = 0.0;
      int satsVal = 0;

      if (isLinked) {
        tempVal = bme.readTemperature();
        humVal  = bme.readHumidity();
        presVal = bme.readPressure() / 100.0F;

        if (gps.location.isValid()) {
          latVal = gps.location.lat();
          lonVal = gps.location.lng();
        }
        if (gps.altitude.isValid())   altVal  = gps.altitude.meters();
        if (gps.speed.isValid())      spdVal  = gps.speed.kmph();
        if (gps.course.isValid())     crsVal  = gps.course.deg();
        if (gps.satellites.isValid()) satsVal = gps.satellites.value();
      }

      // --- CONSTRUCCIÓ DEL JSON ---
      String json = "{";
      
      // MAC HARDCODEDA DIRECTAMENT PER A PROVES
      json += "\"device_id\":\"" + WiFi.macAddress() + "\",";

      json += "\"pair_code\":\"" + pair_code + "\",";
      
      json += "\"lat\":" + String(latVal, 6) + ",";
      json += "\"lon\":" + String(lonVal, 6) + ",";
      json += "\"alt\":" + String(altVal, 2) + ",";
      json += "\"spd\":" + String(spdVal, 2) + ",";
      json += "\"course\":" + String(crsVal, 2) + ",";
      
      json += "\"gravant\":" + String(gravant ? "true" : "false") + ",";
      
      json += "\"temp\":" + String(!isnan(tempVal) ? tempVal : 0.0, 2) + ",";
      json += "\"hum\":" + String(!isnan(humVal) ? humVal : 0.0, 2) + ",";
      json += "\"pres\":" + String(!isnan(presVal) ? presVal : 0.0, 2) + ",";

      if (isLinked && gravant) {
        json += "\"jump_count\":" + String(jumpCount) + ",";
        json += "\"straight_airs\":" + String(countAeriNet) + ",";
        json += "\"jumps_180\":" + String(count180) + ",";
        json += "\"jumps_360\":" + String(count360) + ",";
        json += "\"jumps_540\":" + String(count540) + ",";
        json += "\"jumps_720\":" + String(count720) + ",";
        json += "\"max_airtime\":" + String(maxAirtimeSessio, 2) + ",";
        json += "\"max_spin\":" + String(maxSpinSessio, 0) + ",";
        json += "\"max_landing_g\":" + String(maxLandingG, 2);
      } else {
        json += "\"jump_count\":0,";
        json += "\"straight_airs\":0,";
        json += "\"jumps_180\":0,";
        json += "\"jumps_360\":0,";
        json += "\"jumps_540\":0,";
        json += "\"jumps_720\":0,";
        json += "\"max_airtime\":0.00,";
        json += "\"max_spin\":0,";
        json += "\"max_landing_g\":0.00";
      }
      json += "}";

      // TRACES AL MONITOR SÈRIE
      Serial.println("\n================ [ ENVIAMENT HTTP ] ================");
      Serial.print("ESTAT DISPOSITIU : ");
      if (!isLinked) {
        Serial.println("PENDING (Cap sensor llegit. Gravant bloquejat a FALSE)");
      } else if (gravant) {
        Serial.println("GRAVANT (Llegint GPS, BME280 i IMU/Mpu)");
      } else {
        Serial.println("LINKED (Llegint GPS i BME280. IMU/Mpu a 0)");
      }

      Serial.printf("GPS (Llegit: %s) -> Sats: %d | Lat: %.6f | Lon: %.6f | Alt: %.1fm | Spd: %.1fkm/h\n", 
                    isLinked ? "SI" : "NO (0s)", satsVal, latVal, lonVal, altVal, spdVal);
      Serial.printf("BME (Llegit: %s) -> Temp: %.2fC | Hum: %.2f%% | Pres: %.2fhPa\n", 
                    isLinked ? "SI" : "NO (0s)", tempVal, humVal, presVal);
      Serial.printf("IMU (Directe) -> Accel(g): [X:%.2f, Y:%.2f, Z:%.2f] | Gyro(deg/s): [X:%.1f, Y:%.1f, Z:%.1f] | G-Total: %.2fg\n",
                    axLlegit, ayLlegit, azLlegit, gxLlegit, gyLlegit, gzLlegit, gTotalLlegit);
      Serial.printf("IMU (Processat) -> Jumps: %d | 180s: %d | 360s: %d | MaxAir: %.2fs | MaxG: %.2f\n", 
                    (isLinked && gravant) ? jumpCount : 0, 
                    (isLinked && gravant) ? count180 : 0, 
                    (isLinked && gravant) ? count360 : 0, 
                    (isLinked && gravant) ? maxAirtimeSessio : 0.0, 
                    (isLinked && gravant) ? maxLandingG : 0.0);

      Serial.println("JSON GENERAT:");
      Serial.println(json);
      Serial.println("----------------------------------------------------\n");

      // HTTP POST
      WiFiClientSecure client; 
      client.setInsecure(); 
      HTTPClient http;
      http.begin(client, SERVER_URL); 
      http.addHeader("Content-Type", "application/json");
      
      int code = http.POST(json);
      if (code > 0) {
        String respostaServidor = http.getString();
        Serial.print("RESP. SERVIDOR HTTP ["); Serial.print(code); Serial.print("]: "); 
        Serial.println(respostaServidor);
        
        bool estatAnterior = isLinked;
        isLinked = (respostaServidor.indexOf("linked") != -1);
        
        if (!estatAnterior && isLinked) {
          Serial.println("-> DISPOSITIU VINCULAT AMB ÈXIT!");
          lastSendMs = 0; 
        }
      } else {
        Serial.printf("Error de connexió HTTP: %d\n", code);
      }
      http.end();
    }
  }

  // =======================================================================
  // 5. REFRESC DE LA PANTALLA OLED (Cada 200ms)
  // =======================================================================
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
        display.setTextSize(2); display.setCursor(5, 45); display.print("PC:"); display.print(pair_code);
      }
    }
    display.display(); 
  }
}
