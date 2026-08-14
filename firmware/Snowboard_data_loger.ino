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

// Control d'interrupció i temps del Portal AP
volatile unsigned long ultimTempsInterrupcio = 0;
volatile bool demanaPortalAP = false; 
volatile bool forcaEnviament = false; 
unsigned long tempsIniciPortal = 0;
const unsigned long TEMPS_MAX_PORTAL = 240000; 

// =======================================================================
// VARIABLES DE TELEMETRIA DE SALTS I TRUCS
// =======================================================================
int jumpCount = 0;
float maxAirtimeSessio = 0.0;
float maxSpinSessio = 0.0;
float maxLandingG = 0.0;

// Comptadors de trucs segons la rotació realitzada
int count180 = 0;
int count360 = 0;
int count540 = 0;
int count720 = 0;
int countAeriNet = 0; 

// Variables instantànies del MPU per al printDebug
float last_ax = 0, last_ay = 0, last_az = 0;
float last_gx = 0, last_gy = 0, last_gz = 0;

// =======================================================================
// CONTROL MULTITASCA DUAL CORE (FREERTOS)
// =======================================================================
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t TaskIMUHandle;

// =======================================================================
// TASCA DEDICADA PER A LA IMU A 100 Hz (CORE 0)
// =======================================================================
void TaskIMU(void * pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(10); // 100 Hz

  bool volant = false;
  unsigned long tempsIniciVol = 0;
  unsigned long ultimAterratgeMs = 0; // Debouncing: Temps de refredament post-impacte
  float grausGiratsActuals = 0.0;
  int comptadorMostresEnAire = 0;     // Debouncing: Filtre de mostres consecutives

  for(;;) {
    vTaskDelayUntil(&xLastWakeTime, xFrequency);

    if (gravant) {
      int16_t rawAX, rawAY, rawAZ;
      int16_t rawGX, rawGY, rawGZ;
      mpu.getMotion6(&rawAX, &rawAY, &rawAZ, &rawGX, &rawGY, &rawGZ);

      float ax = (float)rawAX / 2048.0;
      float ay = (float)rawAY / 2048.0;
      float az = (float)rawAZ / 2048.0;
      
      float gx = (float)rawGX / 16.4;
      float gy = (float)rawGY / 16.4;
      float gz = (float)rawGZ / 16.4;

      portENTER_CRITICAL(&timerMux);
      last_ax = ax; last_ay = ay; last_az = az;
      last_gx = gx; last_gy = gy; last_gz = gz;
      portEXIT_CRITICAL(&timerMux);

      float gTotal = sqrt(ax*ax + ay*ay + az*az);
      float dt = 0.010; // 10 ms
      unsigned long ara = millis();

      // 1. DETECCIÓ D'ENLAIRAMENT (< 0.40G)
      // Llindar exigent de 0.4G + Filtre de 3 mostres consecutives + Cooldown de 600 ms post-aterratge
      if (!volant && (ara - ultimAterratgeMs > 600)) {
        if (gTotal < 0.40) { 
          comptadorMostresEnAire++;
          if (comptadorMostresEnAire >= 3) { // Exigeix estabilitat en l'aire (~15ms)
            volant = true;
            tempsIniciVol = ara;
            grausGiratsActuals = 0.0;
            comptadorMostresEnAire = 0;
          }
        } else {
          comptadorMostresEnAire = 0; // Descarta sorolls puntuals
        }
      }

      // 2. INTEGRACIÓ DE GIR MENTRE VOLA
      if (volant) {
        float omegaTotal = sqrt(gx*gx + gy*gy + gz*gz);
        grausGiratsActuals += omegaTotal * dt; 
      }

      // 3. DETECCIÓ D'ATERRATGE (> 1.40G)
      if (volant && gTotal > 1.40) {
        volant = false;
        ultimAterratgeMs = ara; // Inicia el temps de refredament
        comptadorMostresEnAire = 0;

        unsigned long duradaVolMs = ara - tempsIniciVol;

        // Filtre de durada: El salt ha de durar almenys 150 ms per ser considerat vàlid
        if (duradaVolMs >= 150) { 
          float tempsVolSegons = duradaVolMs / 1000.0;

          portENTER_CRITICAL(&timerMux);
          jumpCount++; 
          if (tempsVolSegons > maxAirtimeSessio) maxAirtimeSessio = tempsVolSegons;
          if (grausGiratsActuals > maxSpinSessio) maxSpinSessio = grausGiratsActuals;
          if (gTotal > maxLandingG) maxLandingG = gTotal;

          if (grausGiratsActuals >= 90 && grausGiratsActuals < 270) { count180++; } 
          else if (grausGiratsActuals >= 270 && grausGiratsActuals < 450) { count360++; } 
          else if (grausGiratsActuals >= 450 && grausGiratsActuals < 630) { count540++; } 
          else if (grausGiratsActuals >= 630) { count720++; }
          else if (grausGiratsActuals < 90) { countAeriNet++; }
          portEXIT_CRITICAL(&timerMux);
        }
      }
    } else {
      volant = false;
      comptadorMostresEnAire = 0;
      portENTER_CRITICAL(&timerMux);
      last_ax = 0; last_ay = 0; last_az = 0;
      last_gx = 0; last_gy = 0; last_gz = 0;
      portEXIT_CRITICAL(&timerMux);
    }
  }
}

void IRAM_ATTR manejadorBoto() {
  unsigned long tempsActual = millis();
  
  if (tempsActual - ultimTempsInterrupcio > 300) {
    if (!portalActiu && WiFi.status() == WL_CONNECTED && isLinked) {
      gravant = !gravant; 
      forcaEnviament = true; 
      
      if (gravant) {
        portENTER_CRITICAL_ISR(&timerMux);
        jumpCount = 0;
        countAeriNet = 0;
        count180 = 0;
        count360 = 0;
        count540 = 0;
        count720 = 0;
        maxAirtimeSessio = 0.0;
        maxSpinSessio = 0.0;
        maxLandingG = 0.0;
        portEXIT_CRITICAL_ISR(&timerMux);
      }
    }
    else if (!portalActiu && WiFi.status() != WL_CONNECTED) {
      demanaPortalAP = true; 
    }
    ultimTempsInterrupcio = tempsActual;
  }
}

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

void printDebugLocal(float t, float h, float p, float ax, float ay, float az, float gx, float gy, float gz) {
  portENTER_CRITICAL(&timerMux);
  int _jumps = jumpCount, _c180 = count180, _c360 = count360, _c540 = count540, _cAeri = countAeriNet;
  float _airtime = maxAirtimeSessio, _spin = maxSpinSessio, _landing = maxLandingG;
  portEXIT_CRITICAL(&timerMux);

  Serial.println("\n--- DADES ACTUALS ---");
  Serial.printf("TEMP: %.2f C | HUM: %.2f %% | PRES: %.2f hPa\n", t, h, p);
  Serial.printf("GPS: Lat %.6f, Lon %.6f | Sats: %d\n", gps.location.lat(), gps.location.lng(), gps.satellites.value());
  Serial.printf("ACCEL (G): X: %.2f | Y: %.2f | Z: %.2f\n", ax, ay, az);
  Serial.printf("GYRO (°/s): X: %.2f | Y: %.2f | Z: %.2f\n", gx, gy, gz);
  Serial.printf("TRUCS: Total: %d | 180s: %d | 360s: %d | 540s: %d | Rectes: %d\n", _jumps, _c180, _c360, _c540, _cAeri);
  Serial.printf("RÈCORDS: Max Airtime: %.2fs | Max Spin: %.0f deg | Max Landing: %.2fG\n", _airtime, _spin, _landing);
  Serial.printf("ESTAT: %s\n", gravant ? "GRAVANT" : "STANDBY");
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
  Wire.setClock(400000); // Bus I2C a 400kHz
  
  mpu.initialize();
  if(!mpu.testConnection()) {
    Serial.println("Error de connexió amb MPU6050!");
  }
  mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_16);
  mpu.setFullScaleGyroRange(MPU6050_GYRO_FS_2000);

  bme.begin(0x76);
  gpsSerial.begin(9600, SERIAL_8N1, 16, -1);

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println(F("Error OLED")); 
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2); display.setCursor(15, 20); display.print("WhiteBoX"); 
    display.setTextSize(1); display.setCursor(9, 45); display.print("SNOWBOARD TELEMETRY"); 
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
      delay(500); Serial.print("."); 
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_OFF);
    portalActiu = false;
    Serial.println("\nSense Wi-Fi disponible. Mode Offline actiu.");
  } else {
    portalActiu = false;
  }
  
  display.clearDisplay(); display.display();
  ultimEstatBoto = digitalRead(BOTO_BOOT);
  demanaPortalAP = false; 

  // CREACIÓ DE LA TASCA DEDICADA DE LA IMU EN EL CORE 0
  xTaskCreatePinnedToCore(
    TaskIMU,          
    "IMU_Task",       
    4096,             
    NULL,             
    2,                
    &TaskIMUHandle,   
    0                 
  );
}

void loop() {
  unsigned long currentMillis = millis();

  // PORTAL DE CONFIGURACIÓ
  if (demanaPortalAP && !portalActiu) {
    demanaPortalAP = false; 
    portalActiu = true;
    tempsIniciPortal = currentMillis; 
    WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID); 
    dnsServer.start(53, "*", WiFi.softAPIP()); 
    server.on("/", handleRoot); server.on("/save", HTTP_POST, handleSave); server.onNotFound(handleRoot); 
    server.begin();
  }

  if (portalActiu) {
    if (currentMillis - tempsIniciPortal >= TEMPS_MAX_PORTAL) {
      dnsServer.stop(); server.stop(); WiFi.mode(WIFI_OFF); portalActiu = false;
      return;
    }
    dnsServer.processNextRequest(); server.handleClient(); 
    
    static unsigned long lastOledPortalMs = 0;
    if (currentMillis - lastOledPortalMs > 200) {
      lastOledPortalMs = currentMillis;
      unsigned long segonsRestants = (TEMPS_MAX_PORTAL - (currentMillis - tempsIniciPortal)) / 1000;
      display.clearDisplay(); display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1); display.setCursor(5, 5); display.print("WhiteBoX CONFIG");
      display.setCursor(5, 22); display.print("SSID:" + String(AP_SSID));
      display.setCursor(5, 54); display.print(String(segonsRestants) + "s left");
      display.display();
    }
    return; 
  }

  if (isLinked) {
    while (gpsSerial.available()) { gps.encode(gpsSerial.read()); }
  }

  if (wifi_ssid != "" && WiFi.status() != WL_CONNECTED && (currentMillis - lastSendMs > 15000)) {
    WiFi.mode(WIFI_STA); WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
  }

  unsigned long intervalEnviament = (gravant || !isLinked) ? 5000 : 60000;

  if (forcaEnviament) {
    forcaEnviament = false;
    lastSendMs = currentMillis - intervalEnviament + 100;
  }

  if (currentMillis - lastSendMs >= intervalEnviament) {
    lastSendMs = currentMillis;
    
    if (!isLinked && (currentMillis - lastPairCodeRenewMs > RENEW_PAIR_CODE_INTERVAL)) {
      lastPairCodeRenewMs = currentMillis; generaNouPairCode(); 
    }

    float t = 0.0, h = 0.0, p = 0.0;
    if (isLinked) {
      t = bme.readTemperature(); h = bme.readHumidity(); p = bme.readPressure() / 100.0F;
    }

    float ax_inst = 0.0, ay_inst = 0.0, az_inst = 0.0;
    float gx_inst = 0.0, gy_inst = 0.0, gz_inst = 0.0;

    if (gravant) {
      portENTER_CRITICAL(&timerMux);
      ax_inst = last_ax; ay_inst = last_ay; az_inst = last_az;
      gx_inst = last_gx; gy_inst = last_gy; gz_inst = last_gz;
      portEXIT_CRITICAL(&timerMux);
    }

    printDebugLocal(t, h, p, ax_inst, ay_inst, az_inst, gx_inst, gy_inst, gz_inst); 

    if (WiFi.status() == WL_CONNECTED) {
      portENTER_CRITICAL(&timerMux);
      int _jumps = jumpCount;
      int _sAir = countAeriNet;
      int _c180 = count180, _c360 = count360, _c540 = count540, _c720 = count720;
      float _airtime = maxAirtimeSessio, _spin = maxSpinSessio, _landing = maxLandingG;
      portEXIT_CRITICAL(&timerMux);

      String json = "{";
      json += "\"device_id\":\"" + WiFi.macAddress() + "\",";
      json += "\"pair_code\":\"" + pair_code + "\",";
      json += "\"lat\":" + String(isLinked ? gps.location.lat() : 0.0, 6) + ",";
      json += "\"lon\":" + String(isLinked ? gps.location.lng() : 0.0, 6) + ",";
      json += "\"alt\":" + String(isLinked ? gps.altitude.meters() : 0.0, 2) + ",";
      json += "\"spd\":" + String(isLinked ? gps.speed.kmph() : 0.0, 2) + ",";
      json += "\"course\":" + String(isLinked ? gps.course.deg() : 0.0, 2) + ",";
      json += "\"gravant\":" + String(gravant ? "true" : "false") + ",";
      json += "\"temp\":" + (isLinked && !isnan(t) ? String(t, 2) : "0.00") + ",";
      json += "\"hum\":" + (isLinked && !isnan(h) ? String(h, 2) : "0.00") + ",";
      json += "\"pres\":" + (isLinked && !isnan(p) ? String(p, 2) : "0.00") + ","; 
      
      json += "\"jump_count\":" + String(_jumps) + ",";
      json += "\"straight_airs\":" + String(_sAir) + ",";
      json += "\"jumps_180\":" + String(_c180) + ",";
      json += "\"jumps_360\":" + String(_c360) + ",";
      json += "\"jumps_540\":" + String(_c540) + ",";
      json += "\"jumps_720\":" + String(_c720) + ",";
      json += "\"max_airtime\":" + String(_airtime, 2) + ",";
      json += "\"max_spin\":" + String(_spin, 0) + ",";
      json += "\"max_landing_g\":" + String(_landing, 2);
      json += "}";

      // IMPRIMIR JSON ENVIAT PER CONSOLA SÈRIE
      Serial.print("[HTTP POST] Enviant JSON: ");
      Serial.println(json);

      WiFiClientSecure client; client.setInsecure(); HTTPClient http;
      http.begin(client, SERVER_URL); http.addHeader("Content-Type", "application/json");
      
      int code = http.POST(json);
      if (code > 0) {
        String respostaServidor = http.getString();
        isLinked = (respostaServidor.indexOf("linked") != -1);
      }
      http.end();
    }
  }

  // REFRESC OLED (200ms)
  static unsigned long lastOledRefreshMs = 0;
  if (millis() - lastOledRefreshMs > 200) {
    lastOledRefreshMs = millis();
    display.clearDisplay(); display.setTextColor(SSD1306_WHITE);
    
    if (WiFi.status() != WL_CONNECTED) {
      display.setTextSize(2); display.setCursor(15, 28); display.print("NO-WIFI"); 
    } 
    else {
      if (isLinked) {
        if (gravant) {
          display.setTextSize(2); display.setCursor(0, 20); display.print("RECORDING");
        } else {
          display.setTextSize(2); display.setCursor(15, 22); display.print("linked");
          display.setTextSize(1); display.setCursor(45, 46); display.print("sat: "); display.print(gps.satellites.value());
        }    
      } else {
        display.setTextSize(2); display.setCursor(15, 20); display.print("pending");
        display.setTextSize(2); display.setCursor(5, 45); display.print("PC:"); display.print(pair_code);
      }
    }
    display.display(); 
  }
}
