#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PZEM004Tv30.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_INA219.h>

// --- WiFi & Server ---
const char* WIFI_SSID = "THE METHOD ZONE";
const char* WIFI_PASS = "Chabu321+";
const char* SERVER_URL = "https://solar-powered-stand-alone-management.onrender.com";

// --- Hardware Pins ---
const int RELAY1_PIN = 18;
const int RELAY2_PIN = 4;
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

const float BATTERY_CAPACITY_AH = 70.0;

// --- Sensors ---
LiquidCrystal_I2C lcd(0x27, 20, 4);
Adafruit_INA219 ina219;
HardwareSerial pzemLoadSerial(1);
PZEM004Tv30 pzemPrimary(Serial2, 25, 26);
PZEM004Tv30 pzemLoad(pzemLoadSerial, 16, 17);

// --- Metrics ---
float p_voltage = 0, p_current = 0, p_power = 0, p_energy = 0, p_pf = 0;
float l_voltage = 0, l_current = 0, l_power = 0, l_energy = 0, l_pf = 0;
float batt_voltage = 0, batt_current_A = 0, batt_power_W = 0, batt_soc = 0;

// --- Relay State ---
bool relay1State = true;
bool relay2State = true;

// --- Timing ---
unsigned long lastSend = 0;
unsigned long lastCommandPoll = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastDisplaySwitch = 0;
int displayPage = 0;
int sendCount = 0;
int failCount = 0;

const unsigned long SEND_INTERVAL = 5000;
const unsigned long LCD_INTERVAL = 5000;

float calculateBatterySoC(float voltage) {
  if (voltage >= 12.70) return 100.0;
  if (voltage <= 10.50) return 0.0;
  return constrain(((voltage - 10.50) / (12.70 - 10.50)) * 100.0, 0.0, 100.0);
}

// ========== HTTP with SSL bypass ==========
int httpPOST(const char* url, const char* jsonBody, String& response) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);
  int code = http.POST((uint8_t*)jsonBody, strlen(jsonBody));
  if (code > 0) {
    response = http.getString();
  } else {
    response = "";
  }
  int httpCode = code;
  http.end();
  return httpCode;
}

int httpGET(const char* url, String& response) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(10000);
  int code = http.GET();
  if (code > 0) {
    response = http.getString();
  } else {
    response = "";
  }
  int httpCode = code;
  http.end();
  return httpCode;
}

// ========== Send Sensor Data via GET query params ==========
void sendSensorData() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, skipping send");
    return;
  }

  char url[600];
  snprintf(url, sizeof(url),
    "%s/api/update?"
    "iv=%.1f&ic=%.2f&ip=%.1f&ie=%.2f&ipf=%.2f"
    "&lv=%.1f&lc=%.2f&lp=%.1f&le=%.2f&lpf=%.2f"
    "&bv=%.2f&bc=%.2f&bp=%.2f&bs=%.1f&ba=%.0f"
    "&r1=%s&r2=%s",
    SERVER_URL,
    p_voltage, p_current, p_power, p_energy, p_pf,
    l_voltage, l_current, l_power, l_energy, l_pf,
    batt_voltage, batt_current_A, batt_power_W, batt_soc, BATTERY_CAPACITY_AH,
    relay1State ? "ON" : "OFF",
    relay2State ? "ON" : "OFF"
  );

  String resp;
  int code = httpGET(url, resp);

  sendCount++;
  if (code == 200) {
    failCount = 0;
    Serial.printf("[OK] Data sent (#%d)\n", sendCount);
  } else {
    failCount++;
    Serial.printf("[FAIL] Data send #%d, HTTP: %d\n", sendCount, code);
  }
}

// ========== Poll Commands ==========
void pollCommands() {
  if (WiFi.status() != WL_CONNECTED) return;

  String url = String(SERVER_URL) + "/api/commands";
  String resp;
  int code = httpGET(url.c_str(), resp);

  if (code != 200 || resp.length() == 0) return;

  if (resp.indexOf("\"cmd\":\"relay\"") >= 0) {
    int relay = 0;
    int r1pos = resp.indexOf("\"relay\":");
    if (r1pos >= 0) {
      relay = resp.substring(r1pos + 8, r1pos + 9).toInt();
    }

    bool turnOn = resp.indexOf("\"state\":\"ON\"") >= 0;

    if (relay == 1) {
      digitalWrite(RELAY1_PIN, turnOn ? RELAY_ON : RELAY_OFF);
      relay1State = turnOn;
      Serial.printf(">> Relay 1 -> %s\n", turnOn ? "ON" : "OFF");
    } else if (relay == 2) {
      digitalWrite(RELAY2_PIN, turnOn ? RELAY_ON : RELAY_OFF);
      relay2State = turnOn;
      Serial.printf(">> Relay 2 -> %s\n", turnOn ? "ON" : "OFF");
    }

    char ackUrl[256];
    snprintf(ackUrl, sizeof(ackUrl), "%s/api/relay/ack?relay=%d&state=%s",
      SERVER_URL, relay, turnOn ? "ON" : "OFF");
    String ackResp;
    httpGET(ackUrl, ackResp);
  }
}

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, RELAY_ON);
  digitalWrite(RELAY2_PIN, RELAY_ON);
  relay1State = true;
  relay2State = true;

  Serial.println("==============================================");
  Serial.println("  SOLAR MICROGRID - ESP32 CONTROLLER");
  Serial.println("==============================================");

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi...");

  if (!ina219.begin()) {
    Serial.println("INA219 not found!");
  } else {
    ina219.setCalibration_32V_2A();
    Serial.println("INA219 OK");
  }

  pzemLoadSerial.begin(9600, SERIAL_8N1, 16, 17);
  Serial.println("PZEM sensors initialized");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    attempts++;
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi OK: %s\n", WiFi.localIP().toString().c_str());
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Connected!");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi FAILED!");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi FAILED!");
  }
  delay(1000);
  lcd.clear();
}

// ========== Loop ==========
void loop() {
  unsigned long now = millis();

  // WiFi recovery
  if (now - lastWiFiCheck >= 15000) {
    lastWiFiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi reconnecting...");
      WiFi.reconnect();
    }
  }

  // Send sensor data
  if (now - lastSend >= SEND_INTERVAL) {
    lastSend = now;

    p_voltage = pzemPrimary.voltage();
    p_current = pzemPrimary.current();
    p_power   = pzemPrimary.power();
    p_energy  = pzemPrimary.energy();
    p_pf      = pzemPrimary.pf();
    if (isnan(p_voltage)) { p_voltage = 0; p_current = 0; p_power = 0; p_energy = 0; p_pf = 0; }

    if (p_voltage > 2.0) {
      l_voltage = p_voltage - 2.0;
      l_current = p_current;
      l_pf = (p_pf > 0) ? p_pf : 0.95;
      l_power = l_voltage * l_current * l_pf;
      l_energy += (l_power * (SEND_INTERVAL / 1000.0)) / 3600000.0;
    } else {
      l_voltage = 0; l_current = 0; l_power = 0; l_pf = 0;
    }

    float busvoltage = ina219.getBusVoltage_V();
    float shuntvoltage = ina219.getShuntVoltage_mV();
    batt_voltage    = busvoltage + (shuntvoltage / 1000.0);
    batt_current_A  = ina219.getCurrent_mA() / 1000.0;
    batt_power_W    = ina219.getPower_mW() / 1000.0;
    batt_soc        = calculateBatterySoC(batt_voltage);

    Serial.printf("\n--- Reading #%d (fails:%d) ---\n", sendCount, failCount);
    Serial.printf("WiFi: %s | IP: %s\n",
      WiFi.status() == WL_CONNECTED ? "OK" : "DISCONNECTED",
      WiFi.localIP().toString().c_str());
    Serial.printf("Inv: %.1fV %.2fA %.1fW PF:%.2f\n", p_voltage, p_current, p_power, p_pf);
    Serial.printf("Load: %.1fV %.2fA %.1fW\n", l_voltage, l_current, l_power);
    Serial.printf("Batt: %.2fV %.2fA %.1fW SoC:%.1f%%\n", batt_voltage, batt_current_A, batt_power_W, batt_soc);
    Serial.printf("Relays: R1=%s R2=%s\n", relay1State ? "ON" : "OFF", relay2State ? "ON" : "OFF");

    sendSensorData();
    pollCommands();
  }

  // LCD
  if (now - lastDisplaySwitch >= LCD_INTERVAL) {
    lastDisplaySwitch = now;
    char buf[21];

    switch (displayPage) {
      case 0:
        lcd.setCursor(0, 0); lcd.print("--- INVERTER OUT ---");
        snprintf(buf, sizeof(buf), "V:%5.1fV I:%.2fA     ", p_voltage, p_current); lcd.setCursor(0, 1); lcd.print(buf);
        snprintf(buf, sizeof(buf), "P:%5.1fW  PF:%.2f    ", p_power, p_pf); lcd.setCursor(0, 2); lcd.print(buf);
        snprintf(buf, sizeof(buf), "Sent:%d OK:%d       ", sendCount, sendCount - failCount); lcd.setCursor(0, 3); lcd.print(buf);
        displayPage = 1;
        break;

      case 1:
        lcd.setCursor(0, 0); lcd.print("--- LOAD METRICS ---");
        snprintf(buf, sizeof(buf), "V:%5.1fV I:%.2fA     ", l_voltage, l_current); lcd.setCursor(0, 1); lcd.print(buf);
        snprintf(buf, sizeof(buf), "P:%5.1fW  PF:%.2f    ", l_power, l_pf); lcd.setCursor(0, 2); lcd.print(buf);
        snprintf(buf, sizeof(buf), "Energy:%.2fWh        ", l_energy); lcd.setCursor(0, 3); lcd.print(buf);
        displayPage = 2;
        break;

      case 2:
        lcd.setCursor(0, 0); lcd.print("--- BATTERY -------");
        snprintf(buf, sizeof(buf), "V:%4.2fV I:%.2fA     ", batt_voltage, batt_current_A); lcd.setCursor(0, 1); lcd.print(buf);
        snprintf(buf, sizeof(buf), "P:%4.1fW  SoC:%5.1f%% ", batt_power_W, batt_soc); lcd.setCursor(0, 2); lcd.print(buf);
        snprintf(buf, sizeof(buf), "R1:%s  R2:%s         ", relay1State ? "ON" : "OFF", relay2State ? "ON" : "OFF"); lcd.setCursor(0, 3); lcd.print(buf);
        displayPage = 0;
        break;
    }
  }
}
