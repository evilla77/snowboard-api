#include <TinyGPS++.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include "esp_sleep.h" // Llibreria per al Light Sleep (Gestió d'energia)

// MODIFICACIÓ: Incloem la llibreria de l'MPU6050 que ha funcionat
#include "MPU6050.h"

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

// MODIFICACIÓ: Inicialitzem l'objecte per al sensor MPU6050 clonat
MPU6050 mpu;

//const char* WIFI_SSID = "EV-OnWifi.cat"; 
const char* WIFI_SSID = "vivo Y72 5G";
const char* WIFI_PASS = "onwifimola"; 
const char* SERVER_URL = "https://snowboard-api.onrender.com/upload";

// MODIFICACIÓ MÍNIMA: Canviat el pin BOOT (0) pel nou botó de 3 pins al GPIO 14
const int BOTO_BOOT = 14;       
const int PIN_LED_ESTAT = 2;  
volatile bool gravant = false; // Afegit 'volatile' perquè es modifica de manera asíncrona des de la interrupció         
bool isLinked = false;
int ultimEstatBoto = HIGH;
unsigned long lastSendMs = 0;
String pair_code = "";

// MODIFICACIÓ: Temporitzador ajustat a 5 minuts exactes per renovar el codi de vinculació
unsigned long lastPairCodeRenewMs = 0; 
const unsigned long RENEW_PAIR_CODE_INTERVAL = 300000; // 5 minuts en mil·lisegons (5 * 60 * 1000)

// VARIABLES INTERNES PER AL DEBOUNCE ASÍNCRON DE LA INTERRUPCIÓ
volatile unsigned long ultimTempsInterrupcio = 0;

// FUNCIÓ D'INTERRUPCIÓ (S'executa immediatament per hardware en tocar el botó)
void IRAM_ATTR manejadorBoto() {
  unsigned long tempsActual = millis();
  // Filtre Debounce augmentat a 300ms per garantir estabilitat amb polsadors de 3 pins
  if (tempsActual - ultimTempsInterrupcio > 300) {
    gravant = !gravant; // Commuta l'estat (ON/OFF) de manera permanent fins a la següent pulsió
    ultimTempsInterrupcio = tempsActual;
  }
}

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

// Funció auxiliar per generar un codi nou (per no repetir codi al setup i al loop)
void generaNouPairCode() {
  pair_code = String(esp_random() % 1000000);
  while(pair_code.length() < 6) pair_code = "0" + pair_code;
  Serial.print("--- NOU PAIR CODE GENERAT: ");
  Serial.println(pair_code);
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
  
  // CONFIGURACIÓ DE LA INTERRUPCIÓ NATIVA PER AL BOTÓ AL GPIO 14
  // Canviat a FALLING (flanc de baixada). Només detectarà l'instant en cui el botó és premut cap avall físicament,
  // ignoring completament el moment en cui l'amolles (evitant que actuï com a pulsador momentani).
  attachInterrupt(digitalPinToInterrupt(BOTO_BOOT), manejadorBoto, FALLING);

  // INICI DEL BUS I2C
  Wire.begin(); 
  Wire.setClock(100000); // Velocitat estable per evitar soroll amb el sensor de temp
  
  // MODIFICACIÓ: Inicialització i configuració forçada de l'MPU6050 per saltar el plateau del clon
  Serial.println("-> Inicialitzant el xip MPU6050...");
  mpu.initialize();
  Serial.println("-> Forçant la configuració del sensor clonat...");
  mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_16);
  mpu.setFullScaleGyroRange(MPU6050_GYRO_FS_2000);

  bme.begin(0x76);
  gpsSerial.begin(9600, SERIAL_8N1, 16, -1);
  connectaWiFi();
  
  // Generem el primer codi de l'arrencada
  generaNouPairCode();
  lastPairCodeRenewMs = millis(); // Inicialitzem el comptador del temps

  // MODIFICACIÓ: Engeguem la pantalla OLED com a l'origen amb el missatge "hola rider"
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println(F("Error: No s'ha pogut iniciar l'OLED"));
  } else {
    display.clearDisplay();      // Neteja la memòria de la pantalla
    display.setTextSize(2);      // Mida de lletra gran
    display.setTextColor(SSD1306_WHITE); // Color blanc
    display.setCursor(10, 20);   // Posició a la pantalla (X, Y)
    display.print("Welcome\n  rider"); 
    display.display();           // Envia les dades físicament a la pantalla per pintar-les
  }
  
  ultimEstatBoto = digitalRead(BOTO_BOOT);
}

void loop() {
  // 1. LLEGIR GPS
  while (gpsSerial.available()) { gps.encode(gpsSerial.read()); }
  unsigned long currentMillis = millis();

  // 2. LECTURA DEL BOTÓ (Eliminada d'aquí per passar a executar-se per Hardware a la funció manejadorBoto d'interrupció nativa)
  // D'aquesta manera el botó respon l'instant exacte fins i tot si la placa envia per Wi-Fi o processa dades.

  // Intenta reconnectar automàticament si s'ha perdut la línia de forma asíncrona
  if (WiFi.status() != WL_CONNECTED && (currentMillis - lastSendMs > 10000)) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }

  // 3. ENVIAMENT DE DADES (CADA 5 SEGONS)
  if (currentMillis - lastSendMs > 5000) {
    lastSendMs = currentMillis;
    
    // MODIFICACIÓ PROTECTORA: El temps del pair_code només es revisa sincronitzat dins dels 5s de l'enviament
    if (!isLinked && (currentMillis - lastPairCodeRenewMs > RENEW_PAIR_CODE_INTERVAL)) {
      lastPairCodeRenewMs = currentMillis;
      generaNouPairCode(); // Genera un codi totalment nou de 6 dígits cada 5 minuts
    }

    float t = bme.readTemperature();
    float h = bme.readHumidity();
    float p = bme.readPressure() / 100.0F;

    // MODIFICACIÓ: Llegim les variables de l'MPU6050 abans d'armar el JSON
    int16_t rawAX, rawAY, rawAZ;
    int16_t rawGX, rawGY, rawGZ;
    mpu.getMotion6(&rawAX, &rawAY, &rawAZ, &rawGX, &rawGY, &rawGZ);

    float accX = (float)rawAX / 2048.0;
    float accY = (float)rawAY / 2048.0;
    float accZ = (float)rawAZ / 2048.0;

    float gyroX = (float)rawGX / 16.4;
    float gyroY = (float)rawGY / 16.4;
    float gyroZ = (float)rawGZ / 16.4;

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
      // MODIFICACIÓ: S'han comentat les 6 variables perquè el servidor no està preparat, així no el trenquem
      //json += "\"accX\":" + String(accX, 2) + ",";
      //json += "\"accY\":" + String(accY, 2) + ",";
      //json += "\"accZ\":" + String(accZ, 2) + ",";
      //json += "\"gyroX\":" + String(gyroX, 2) + ",";
      //json += "\"gyroY\":" + String(gyroY, 2) + ",";
      //json += "\"gyroZ\":" + String(gyroZ, 2);
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
        String respostaServidor = http.getString();
        Serial.print("--- RESP_SERVIDOR: ");
        Serial.println(respostaServidor);

        // CORREGIT: Comprovació explícita per assignar true o false real segons la resposta del servidor
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

  // MODIFICACIÓ CRÍTICA: Lògica de pantalles sol·licitada (linked, RECORDING o pendint) extreta fora del delay d'enviament de dades
  // S'executa asíncronament amb un refresc curt de seguretat (200ms) per evitar parpelleig a l'OLED, responent instantàniament al botó.
  static unsigned long lastOledRefreshMs = 0;
  if (millis() - lastOledRefreshMs > 200) {
    lastOledRefreshMs = millis();

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    
    // CORREGIT: Ajustat exactament al disseny sol·licitat tot dins de la zona blava (A partir de Y = 28)
    if (WiFi.status() != WL_CONNECTED) {
      // NO Wi-Fi en mida 2
      display.setTextSize(2);    
      display.setCursor(15, 28); 
      display.print("NO-WIFI"); 

      // pending a seques en mida 1 a sota
      display.setTextSize(1);
      display.setCursor(43, 50); // Centrat a sota en la línia blava inferior
      display.print("searching");
    } 
    // Si tenim WiFi correctament, passem a avaluar els estats habituals
    else {
      if (isLinked) {
        display.setTextSize(2);
        display.setCursor(0, 20);
        
        // Si està vinculat, mirem si l'usuari ha premut el botó de gravar
        if (gravant) {
          display.print("RECORDING"); // Text en anglès quan grava
        } else {
          display.print("linked");    // Torna a linked si parem de gravar
        }
      } else {
        display.setTextSize(2);
        display.setCursor(15, 25); 
        display.print("pending");

        // Segona línia: El codi separat i ben espaiat més avall (Y = 48)
        display.setTextSize(2);
        display.setCursor(15, 48); // Augmentant aquest número (ex: 48, 50...) i baixes el codi al teu gust
        display.print("Cod:");
        display.print(pair_code);
      }
    }
    display.display(); // Actualitza la pantalla amb els nous canvis reals
  }
}