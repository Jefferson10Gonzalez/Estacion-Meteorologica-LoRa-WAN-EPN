#include <heltec_unofficial.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <SensirionI2cScd4x.h>

//  SENSORES PMS5003 y SCD41

// PMS5003 - UART
// TXD → Heltec RX (GPIO44), RXD → Heltec TX (GPIO43), VCC → 5V, GND → GND
#define PMS_RX 44
#define PMS_TX 43
HardwareSerial pmsSerial(1);

struct PMS5003Data {
  uint16_t pm1_0;   // µg/m³ ambiental
  uint16_t pm2_5;   // µg/m³ ambiental
  uint16_t pm10;    // µg/m³ ambiental
};

bool readPMS5003(PMS5003Data &pms) {
  uint8_t buf[32];
  if (pmsSerial.available() < 32) return false;
  if (pmsSerial.read() != 0x42) return false;
  if (pmsSerial.read() != 0x4D) return false;
  pmsSerial.readBytes(buf, 30);
  pms.pm1_0 = (buf[4] << 8) | buf[5];
  pms.pm2_5 = (buf[6] << 8) | buf[7];
  pms.pm10  = (buf[8] << 8) | buf[9];
  return true;
}

// SCD41 - I2C
// VCC → 3.3V, GND → GND, SDA → GPIO18, SCL → GPIO17
#define SDA_PIN 18
#define SCL_PIN 17
SensirionI2cScd4x scd41;

bool readSCD41(uint16_t &co2) {
  uint16_t co2Raw, temp, hum;
  bool isReady = false;
  scd41.getDataReadyStatus(isReady);
  if (!isReady) return false;
  scd41.readMeasurement(co2Raw, temp, hum);
  co2 = co2Raw;
  return true;
}

//  ESTRUCTURA DE DATOS
struct WeatherData {
  float temp_out;    // Temperatura exterior (°C)
  float humidity;    // Humedad exterior (%)
  float pressure;    // Presión relativa (hPa)
  float wind_speed;  // Velocidad viento (m/s)
  float wind_gust;   // Ráfaga viento (m/s)
  int   wind_dir;    // Dirección viento (°)
  float rain_rate;   // Tasa de lluvia (mm/h)
  float rain_day;    // Lluvia acumulada día (mm)
  float uv;          // Índice UV
  float solar;       // Radiación solar (W/m²)
};

struct AirQualityData {
  uint16_t pm1_0;   // PM1.0 µg/m³ × 10
  uint16_t pm2_5;   // PM2.5 µg/m³ × 10
  uint16_t pm10;    // PM10 µg/m³ × 10
  uint16_t co2;     // CO2 ppm
};

//  CREDENCIALES LoRaWAN TTN
uint64_t joinEUI = 0x0000000000000002;
uint64_t devEUI  = 0x70B3D57ED00772F7;

uint8_t appKey[] = {
  <APP_KEY_BYTE_0>, <APP_KEY_BYTE_1>, <APP_KEY_BYTE_2>, <APP_KEY_BYTE_3>,
  <APP_KEY_BYTE_4>, <APP_KEY_BYTE_5>, <APP_KEY_BYTE_6>, <APP_KEY_BYTE_7>,
  <APP_KEY_BYTE_8>, <APP_KEY_BYTE_9>, <APP_KEY_BYTE_10>, <APP_KEY_BYTE_11>,
  <APP_KEY_BYTE_12>, <APP_KEY_BYTE_13>, <APP_KEY_BYTE_14>, <APP_KEY_BYTE_15>
};
uint8_t nwkKey[] = {
  <NWK_KEY_BYTE_0>, <NWK_KEY_BYTE_1>, <NWK_KEY_BYTE_2>, <NWK_KEY_BYTE_3>,
  <NWK_KEY_BYTE_4>, <NWK_KEY_BYTE_5>, <NWK_KEY_BYTE_6>, <NWK_KEY_BYTE_7>,
  <NWK_KEY_BYTE_8>, <NWK_KEY_BYTE_9>, <NWK_KEY_BYTE_10>, <NWK_KEY_BYTE_11>,
  <NWK_KEY_BYTE_12>, <NWK_KEY_BYTE_13>, <NWK_KEY_BYTE_14>, <NWK_KEY_BYTE_15>
};

LoRaWANNode node(&radio, &US915, 2);
Preferences store;

//  CONFIGURACION WiFi
const char* WIFI_SSID     = "<WIFI_SSID>";
const char* WIFI_PASSWORD = "<WIFI_PASSWORD>";

//  API LOCAL RADDY L7
// IP fija asignada en la red local - actualizar si cambia
const char* RADDY_IP  = "<RADDY_IP>";
const char* RADDY_URL = "http://<RADDY_IP>/client?command=record";

const uint32_t TX_INTERVAL_MS  = 60000;   // Intervalo de transmisión (60 segundos)
const uint32_t JOIN_BACKOFF_MS = 30000;   // Espera entre reintentos de join

uint32_t lastTx = 0;

//  WiFi: conectar / reconectar
bool ensureWiFi(uint32_t timeoutMs = 10000) {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.print("Conectando WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(300);
    Serial.print(".");
    heltec_loop();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi OK - IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("\nWiFi fallido");
  return false;
}

//  LEER API LOCAL RADDY L7
// Endpoint: /client?command=record
// 
// Estructura JSON esperada:
//   sensor[0] = Indoor  (temp, humedad interior)
//   sensor[1] = Outdoor (temp, humedad exterior)
//   sensor[2] = Pressure (absoluta, relativa)
//   sensor[3] = Wind Speed (rafaga max, viento, rafaga, direccion, avg2m, dir2m, avg10m, dir10m)
//   sensor[4] = Rainfall (rate, hora, dia, semana, mes, año, total)
//   sensor[5] = Solar (radiacion, UV)
//
// Velocidad de viento viene en km/h → convertir a m/s dividiendo entre 3.6
bool fetchWeather(WeatherData &wd) {
  if (!ensureWiFi()) return false;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);

  if (!http.begin(client, RADDY_URL)) {
    Serial.println("http.begin() fallo");
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    Serial.printf("RADDY L7 HTTP error: %d\n", code);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();

  if (err) {
    Serial.printf("JSON error: %s\n", err.c_str());
    return false;
  }

  JsonArray sensor = doc["sensor"];

  // sensor[1] = Outdoor: [["Temperature","19.4","°C"],["Humidity","55","%"]]
  wd.temp_out = atof(sensor[1]["list"][0][1]);
  wd.humidity = atof(sensor[1]["list"][1][1]);

  // sensor[2] = Pressure: [["Absolute","692.4","hpa"],["Relative","1013.1","hpa"]]
  wd.pressure = atof(sensor[2]["list"][1][1]);  // Presión relativa

  // sensor[3] = Wind Speed (valores en km/h → convertir a m/s)
  // list[1] = Wind, list[2] = Gust, list[3] = Direction
  wd.wind_speed = atof(sensor[3]["list"][1][1]) / 3.6f;
  wd.wind_gust  = atof(sensor[3]["list"][2][1]) / 3.6f;
  wd.wind_dir   = atoi(sensor[3]["list"][3][1]);

  // sensor[4] = Rainfall
  // list[0] = Rate (mm/hr), list[2] = Day (mm)
  wd.rain_rate = atof(sensor[4]["list"][0][1]);
  wd.rain_day  = atof(sensor[4]["list"][2][1]);

  // sensor[5] = Solar
  // list[0] = Light (W/m²), list[1] = UVI
  wd.solar = atof(sensor[5]["list"][0][1]);
  wd.uv    = atof(sensor[5]["list"][1][1]);

  return true;
}

//  CALIDAD DEL AIRE (LECTURA REAL: PMS5003 + SCD41)
void readAirQuality(AirQualityData &aq) {
  // PMS5003
  PMS5003Data pms;
  if (readPMS5003(pms)) {
    aq.pm1_0 = pms.pm1_0 * 10;   // × 10 para mantener 1 decimal en el payload
    aq.pm2_5 = pms.pm2_5 * 10;
    aq.pm10  = pms.pm10  * 10;
  } else {
    Serial.println("PMS5003: sin datos disponibles en este ciclo.");
    aq.pm1_0 = 0;
    aq.pm2_5 = 0;
    aq.pm10  = 0;
  }

  // SCD41
  uint16_t co2Raw = 0;
  if (readSCD41(co2Raw)) {
    aq.co2 = co2Raw;
  } else {
    Serial.println("SCD41: medicion aun no lista en este ciclo.");
    aq.co2 = 0;
  }
}

//  EMPAQUETAR PAYLOAD (28 bytes)
//
//  Bytes  0-1  : Temperatura × 10        (int16,  °C × 10)
//  Bytes  2-3  : Humedad                  (uint16, %)
//  Bytes  4-5  : Presion × 10            (uint16, hPa × 10)
//  Bytes  6-7  : Velocidad viento × 10   (uint16, m/s × 10)
//  Bytes  8-9  : Rafaga viento × 10      (uint16, m/s × 10)
//  Bytes 10-11 : Direccion viento        (uint16, °)
//  Bytes 12-13 : Tasa lluvia × 10        (uint16, mm/h × 10)
//  Bytes 14-15 : Lluvia dia × 10         (uint16, mm × 10)
//  Byte   16   : Indice UV               (uint8)
//  Byte   17   : Radiacion solar / 4     (uint8, W/m² / 4)
//  Bytes 18-19 : PM1.0 × 10             (uint16, µg/m³ × 10)
//  Bytes 20-21 : PM2.5 × 10             (uint16, µg/m³ × 10)
//  Bytes 22-23 : PM10 × 10              (uint16, µg/m³ × 10)
//  Bytes 24-25 : CO2                    (uint16, ppm)
//  Bytes 26-27 : Reservado              (uint16)
void buildPayload(WeatherData &wd, AirQualityData &aq, uint8_t* buf) {
  int16_t  t   = (int16_t)(wd.temp_out   * 10);
  uint16_t h   = (uint16_t)(wd.humidity);
  uint16_t p   = (uint16_t)(wd.pressure  * 10);
  uint16_t ws  = (uint16_t)(wd.wind_speed * 10);
  uint16_t wg  = (uint16_t)(wd.wind_gust  * 10);
  uint16_t wr  = (uint16_t)(wd.wind_dir);
  uint16_t rr  = (uint16_t)(wd.rain_rate  * 10);
  uint16_t rd  = (uint16_t)(wd.rain_day   * 10);
  uint8_t  uv  = (uint8_t)constrain((int)wd.uv, 0, 255);
  uint8_t  sr  = (uint8_t)constrain((int)(wd.solar / 4.0f), 0, 255);

  buf[0]=t>>8;    buf[1]=t&0xFF;
  buf[2]=h>>8;    buf[3]=h&0xFF;
  buf[4]=p>>8;    buf[5]=p&0xFF;
  buf[6]=ws>>8;   buf[7]=ws&0xFF;
  buf[8]=wg>>8;   buf[9]=wg&0xFF;
  buf[10]=wr>>8;  buf[11]=wr&0xFF;
  buf[12]=rr>>8;  buf[13]=rr&0xFF;
  buf[14]=rd>>8;  buf[15]=rd&0xFF;
  buf[16]=uv;     buf[17]=sr;

  // Calidad del aire
  buf[18]=aq.pm1_0>>8;  buf[19]=aq.pm1_0&0xFF;
  buf[20]=aq.pm2_5>>8;  buf[21]=aq.pm2_5&0xFF;
  buf[22]=aq.pm10>>8;   buf[23]=aq.pm10&0xFF;
  buf[24]=aq.co2>>8;    buf[25]=aq.co2&0xFF;

  // Reservado
  buf[26]=0; buf[27]=0;
}

//  OLED: mostrar PM2.5 y CO2
void showAirQualityOLED(AirQualityData &aq) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0,  "Estacion Meteorologica");
  display.drawString(0, 16, "PM2.5: " + String(aq.pm2_5 / 10.0f, 1) + " ug/m3");
  display.drawString(0, 32, "CO2: " + String(aq.co2) + " ppm");
  display.display();
}

//  LoRaWAN: activar con persistencia NVS
int16_t lwActivate() {
  node.beginOTAA(joinEUI, devEUI, nwkKey, appKey);
  node.setADR(true);

  store.begin("radiolib", false);

  // Intentamos restaurar sesión previa si existe
  if (store.isKey("nonces")) {
    uint8_t nb[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
    store.getBytes("nonces", nb, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
    node.setBufferNonces(nb);

    if (store.isKey("session")) {
      uint8_t sb[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];
      store.getBytes("session", sb, RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
      node.setBufferSession(sb);
    }

    Serial.println("Reanudando sesion guardada...");
    int16_t st = node.activateOTAA();
    if (st == RADIOLIB_LORAWAN_SESSION_RESTORED) {
      Serial.println("Sesion restaurada (sin re-join).");
      return st;
    }
    Serial.printf("No se pudo reanudar (%d), se hara Join nuevo.\n", st);
  } else {
    Serial.println("Primer arranque: sin nonces guardados.");
  }

  // Si no hay sesión válida, hacemos join completo
  int16_t st = RADIOLIB_ERR_NETWORK_NOT_JOINED;
  while (st != RADIOLIB_LORAWAN_NEW_SESSION) {
    Serial.println("Enviando Join-Request a TTN...");
    st = node.activateOTAA();

    uint8_t nb[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
    memcpy(nb, node.getBufferNonces(), RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
    store.putBytes("nonces", nb, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);

    if (st != RADIOLIB_LORAWAN_NEW_SESSION) {
      Serial.printf("Join fallido (%d). Reintento en %lus...\n",
                    st, JOIN_BACKOFF_MS / 1000);
      heltec_loop();
      delay(JOIN_BACKOFF_MS);
    }
  }

  Serial.println("Unido a TTN! (nueva sesion)");
  uint8_t sb[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];
  memcpy(sb, node.getBufferSession(), RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
  store.putBytes("session", sb, RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
  return st;
}

void saveSession() {
  uint8_t sb[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];
  memcpy(sb, node.getBufferSession(), RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
  store.putBytes("session", sb, RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
}

//  SETUP
void setup() {
  heltec_setup();
  delay(1000);

  Serial.println("\n=== Estacion Meteorologica RADDY L7 ===");
  Serial.println("    Heltec WiFi LoRa 32 V3 - US915 FSB2");
  Serial.printf("    Fuente de datos: %s\n", RADDY_URL);

  // Inicializar sensores reales
  Wire.begin(SDA_PIN, SCL_PIN);
  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX, PMS_TX);
  scd41.begin(Wire, SCD41_I2C_ADDR_62);
  scd41.startPeriodicMeasurement();
  delay(5000);                 // SCD41 necesita 5s para primera medicion
  Serial.println("Sensores PMS5003 y SCD41 iniciados.");

  ensureWiFi();

  Serial.print("radio.begin(): ");
  int16_t rs = radio.begin();
  Serial.println(rs);
  if (rs != RADIOLIB_ERR_NONE) {
    Serial.println("Fallo el radio. Deteniendo.");
    while (true) { heltec_loop(); delay(1000); }
  }

  Serial.println("Activando LoRaWAN (OTAA)...");
  lwActivate();

  lastTx = millis() - TX_INTERVAL_MS;  // Forzar primer envio inmediato
}

//  LOOP
void loop() {
  heltec_loop();

  if (millis() - lastTx < TX_INTERVAL_MS) return;
  lastTx = millis();

  WeatherData wd;
  AirQualityData aq;
  uint8_t txBuf[28];

  if (!fetchWeather(wd)) {
    Serial.println("No se pudieron leer datos de RADDY L7, se omite este ciclo.");
    return;
  }

  // Leer calidad del aire (sensores reales: PMS5003 + SCD41)
  readAirQuality(aq);

  // Mostrar datos en Monitor Serial
  Serial.println("─────────────────────────────────");
  Serial.printf("Temp: %.1f C | Hum: %.0f %% | Pres: %.1f hPa\n",
                wd.temp_out, wd.humidity, wd.pressure);
  Serial.printf("Viento: %.1f m/s (%.1f km/h) | Rafaga: %.1f m/s (%.1f km/h) | Dir: %d\n",
                wd.wind_speed, wd.wind_speed * 3.6f,
                wd.wind_gust,  wd.wind_gust  * 3.6f, wd.wind_dir);
  Serial.printf("Lluvia: %.1f mm/h (%.1f mm dia) | UV: %.1f | Solar: %.2f W/m2\n",
                wd.rain_rate, wd.rain_day, wd.uv, wd.solar);
  Serial.println("--- Calidad del Aire ---");
  Serial.printf("PM1.0: %.1f ug/m3 | PM2.5: %.1f ug/m3 | PM10: %.1f ug/m3\n",
                aq.pm1_0 / 10.0f, aq.pm2_5 / 10.0f, aq.pm10 / 10.0f);
  Serial.printf("CO2: %d ppm\n", aq.co2);
  Serial.println("─────────────────────────────────");

  // Mostrar PM2.5 y CO2 en el OLED integrado
  showAirQualityOLED(aq);

  buildPayload(wd, aq, txBuf);

  Serial.println("Transmitiendo por LoRaWAN...");
  int16_t state = node.sendReceive(txBuf, sizeof(txBuf), 1);

  if (state >= RADIOLIB_ERR_NONE) {
    Serial.printf("OK -> TTN%s\n", state > 0 ? " (downlink recibido)" : "");
    saveSession();
  } else {
    Serial.printf("TX error: %d\n", state);
  }

  Serial.printf("Proximo envio en %lus\n\n", TX_INTERVAL_MS / 1000);
}
