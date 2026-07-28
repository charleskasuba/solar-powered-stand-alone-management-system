#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PZEM004Tv30.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_INA219.h>

// --- WiFi & Server Configuration ---
const char* WIFI_SSID = "THE METHOD ZONE";
const char* WIFI_PASS = "Chabu321+";
const char* SERVER_URL = "https://solar-powered-stand-alone-management.onrender.com";

// --- Hardware Pins ---
const int RELAY1_PIN = 18; // IN1: Critical Load (Always ON)
const int RELAY2_PIN = 16; // IN2: Non-Critical / Auxiliary Load

#define RELAY_ON  LOW
#define RELAY_OFF HIGH

const float BATTERY_CAPACITY_AH = 70.0;

// --- Display & Hardware Modules ---
LiquidCrystal_I2C lcd(0x27, 20, 4);
Adafruit_INA219 ina219;

// Primary Inverter PZEM (HardwareSerial 2: RX=25, TX=26)
PZEM004Tv30 pzemPrimary(Serial2, 25, 26);

// Load PZEM (HardwareSerial 1: Moved to RX=32, TX=33 to clear GPIO 16 for Relay 2)
HardwareSerial pzemLoadSerial(1);
PZEM004Tv30 pzemLoad(pzemLoadSerial, 32, 33);

// --- Metrics Variables ---
float p_voltage = 0, p_current = 0, p_power = 0, p_energy = 0, p_pf = 0;
float l_voltage = 0, l_current = 0, l_power = 0, l_energy = 0, l_pf = 0;
float batt_voltage = 0, batt_current_A = 0, batt_power_W = 0, batt_soc = 0;

// --- Relay States ---
bool relay1State = true; // Critical Load: forced true
bool relay2State = true;

// --- Timing ---
unsigned long lastSend = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastDisplaySwitch = 0;
int displayPage = 0;
int sendCount = 0;
int failCount = 0;

const unsigned long SEND_INTERVAL = 5000;
const unsigned long LCD_INTERVAL = 4000;

// Lead-Acid State of Charge Approximation
float calculateBatterySoC(float voltage) {
  if (voltage >= 12.70) return 100.0;
  if (voltage <= 10.50) return 0.0;
  return constrain(((voltage - 10.50) / (12.70 - 10.50)) * 100.0, 0.0, 100.0);
}

// ========== Send Sensor Data via HTTP GET ==========
void sendSensorData() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] WiFi disconnect, skipping send.");
    return;
  }

  String url = String(SERVER_URL) + "/api/update?";
  url += "iv=" + String(p_voltage, 1);
  url += "&ic=" + String(p_current, 2);
  url += "&ip=" + String(p_power, 1);
  url += "&ie=" + String(p_energy, 2);
  url += "&ipf=" + String(p_pf, 2);
  url += "&lv=" + String(l_voltage, 1);
  url += "&lc=" + String(l_current, 2);
  url += "&lp=" + String(l_power, 1);
  url += "&le=" + String(l_energy, 2);
  url += "&lpf=" + String(l_pf, 2);
  url += "&bv=" + String(batt_voltage, 2);
  url += "&bc=" + String(batt_current_A, 2);
  url += "&bp=" + String(batt_power_W, 2);
  url += "&bs=" + String(batt_soc, 1);
  url += "&ba=" + String(BATTERY_CAPACITY_AH, 0);
  url += "&r1=" + String(relay1State ? "ON" : "OFF");
  url += "&r2=" + String(relay2State ? "ON" : "OFF");

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(8000);

  int code = http.GET();
  sendCount++;

  if (code > 0) {
    failCount = 0;
    Serial.printf("[OK] Data sent (#%d), HTTP Code: %d\n", sendCount, code);
  } else {
    failCount++;
    Serial.printf("[FAIL] Data send #%d, Error: %s (%d)\n", sendCount, http.errorToString(code).c_str(), code);
  }
  http.end();
}

// ========== Poll Server Commands ==========
void pollCommands() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(SERVER_URL) + "/api/commands";
  http.begin(client, url);
  http.setTimeout(5000);

  int code = http.GET();
  if (code == 200) {
    String resp = http.getString();
    if (resp.indexOf("\"cmd\":\"relay\"") >= 0) {
      int relay = 0;
      int r1pos = resp.indexOf("\"relay\":");
      if (r1pos >= 0) {
        relay = resp.substring(r1pos + 8, r1pos + 9).toInt();
      }

      bool turnOn = resp.indexOf("\"state\":\"ON\"") >= 0;

      if (relay == 1) {
        // ENFORCE CRITICAL LOAD PROTECTION: Always remain ON regardless of server command
        digitalWrite(RELAY1_PIN, RELAY_ON);
        relay1State = true;
        Serial.println(">> Relay 1 override: Critical load forced ALWAYS ON");
      } else if (relay == 2) {
        digitalWrite(RELAY2_PIN, turnOn ? RELAY_ON : RELAY_OFF);
        relay2State = turnOn;
        Serial.printf(">> Relay 2 commanded -> %s\n", turnOn ? "ON" : "OFF");
      }
    }
  }
  http.end();
}

// ========== Refresh LCD Interface ==========
void updateLCDScreen() {
  char buf[21];
  lcd.clear();

  switch (displayPage) {
    case 0:
      lcd.setCursor(0, 0); lcd.print("--- INVERTER OUT ---");
      snprintf(buf, sizeof(buf), "V:%5.1fV  I:%5.2fA", p_voltage, p_current);
      lcd.setCursor(0, 1); lcd.print(buf);
      snprintf(buf, sizeof(buf), "P:%5.1fW PF:%4.2f", p_power, p_pf);
      lcd.setCursor(0, 2); lcd.print(buf);
      snprintf(buf, sizeof(buf), "Sent:%d  OK:%d", sendCount, sendCount - failCount);
      lcd.setCursor(0, 3); lcd.print(buf);
      displayPage = 1;
      break;

    case 1:
      lcd.setCursor(0, 0); lcd.print("--- LOAD METRICS ---");
      snprintf(buf, sizeof(buf), "V:%5.1fV  I:%5.2fA", l_voltage, l_current);
      lcd.setCursor(0, 1); lcd.print(buf);
      snprintf(buf, sizeof(buf), "P:%5.1fW PF:%4.2f", l_power, l_pf);
      lcd.setCursor(0, 2); lcd.print(buf);
      snprintf(buf, sizeof(buf), "Energy:%6.2fWh", l_energy);
      lcd.setCursor(0, 3); lcd.print(buf);
      displayPage = 2;
      break;

    case 2:
      lcd.setCursor(0, 0); lcd.print("--- BATTERY METRICS ---");
      snprintf(buf, sizeof(buf), "V:%5.2fV  I:%5.2fA", batt_voltage, batt_current_A);
      lcd.setCursor(0, 1); lcd.print(buf);
      snprintf(buf, sizeof(buf), "P:%5.1fW  SoC:%4.1f%%", batt_power_W, batt_soc);
      lcd.setCursor(0, 2); lcd.print(buf);
      snprintf(buf, sizeof(buf), "R1:%s R2:%s", relay1State ? "ON" : "OFF", relay2State ? "ON" : "OFF");
      lcd.setCursor(0, 3); lcd.print(buf);
      displayPage = 0;
      break;
  }
}

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize Relays
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  
  digitalWrite(RELAY1_PIN, RELAY_ON); // Lock IN1 ON for critical loads
  digitalWrite(RELAY2_PIN, RELAY_ON); // Default IN2 ON
  
  relay1State = true;
  relay2State = true;

  Serial.println("\n==============================================");
  Serial.println("   SOLAR MICROGRID - ESP32 CONTROLLER");
  Serial.println("==============================================");

  // I2C & LCD Initialization
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("System Booting...");
  lcd.setCursor(0, 1);
  lcd.print("Connecting WiFi...");

  // INA219 Setup
  if (!ina219.begin()) {
    Serial.println("[WARN] INA219 not detected on I2C!");
  } else {
    ina219.setCalibration_32V_2A();
    Serial.println("[OK] INA219 Initialized");
  }

  // Load PZEM Serial Setup (GPIO 32 RX, GPIO 33 TX)
  pzemLoadSerial.begin(9600, SERIAL_8N1, 32, 33);
  Serial.println("[OK] PZEM Serial Interfaces Configured");

  // WiFi Connection Procedure
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
    Serial.print(".");
  }

  lcd.clear();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[OK] WiFi Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    lcd.setCursor(0, 0);
    lcd.print("WiFi Connected!");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());
  } else {
    Serial.println("\n[FAIL] WiFi Connection Failed!");
    lcd.setCursor(0, 0);
    lcd.print("WiFi Failed!");
    lcd.setCursor(0, 1);
    lcd.print("Running Offline...");
  }
  delay(2000);
  lcd.clear();
}

// ========== Main Loop ==========
void loop() {
  unsigned long now = millis();

  // 1. WiFi Reconnection Handler
  if (now - lastWiFiCheck >= 15000) {
    lastWiFiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Reconnecting...");
      WiFi.reconnect();
    }
  }

  // 2. Read Sensors and Communicate
  if (now - lastSend >= SEND_INTERVAL) {
    lastSend = now;

    // Direct Read: Inverter PZEM (Primary)
    float temp_v = pzemPrimary.voltage();
    float temp_i = pzemPrimary.current();
    float temp_p = pzemPrimary.power();
    float temp_e = pzemPrimary.energy();
    float temp_pf = pzemPrimary.pf();

    if (isnan(temp_v) || temp_v < 0) {
      p_voltage = 0.0; p_current = 0.0; p_power = 0.0; p_pf = 0.0;
      Serial.println("[PZEM] Primary read error / AC offline.");
    } else {
      p_voltage = temp_v;
      p_current = temp_i;
      p_power   = temp_p;
      p_energy  = temp_e;
      p_pf      = temp_pf;
    }

    // Direct Read: Dedicated Load PZEM
    float l_temp_v = pzemLoad.voltage();
    float l_temp_i = pzemLoad.current();
    float l_temp_p = pzemLoad.power();
    float l_temp_e = pzemLoad.energy();
    float l_temp_pf = pzemLoad.pf();

    if (!isnan(l_temp_v) && l_temp_v > 0) {
      l_voltage = l_temp_v;
      l_current = l_temp_i;
      l_power   = l_temp_p;
      l_energy  = l_temp_e;
      l_pf      = l_temp_pf;
    } else if (p_voltage > 10.0) {
      // Fallback: Derivation if second PZEM bus is offline
      l_voltage = p_voltage - 2.0;
      l_current = p_current;
      l_pf = (p_pf > 0) ? p_pf : 0.95;
      l_power = l_voltage * l_current * l_pf;
      l_energy += (l_power * (SEND_INTERVAL / 1000.0)) / 3600000.0;
    } else {
      l_voltage = 0; l_current = 0; l_power = 0; l_pf = 0;
    }

    // Read Battery DC Metrics (INA219)
    float busvoltage = ina219.getBusVoltage_V();
    float shuntvoltage = ina219.getShuntVoltage_mV();
    float current_mA = ina219.getCurrent_mA();
    float power_mW = ina219.getPower_mW();

    if (isnan(busvoltage) || isinf(busvoltage) || busvoltage <= 0.0) {
      batt_voltage = 0.0;
      batt_current_A = 0.0;
      batt_power_W = 0.0;
      batt_soc = 0.0;
    } else {
      batt_voltage = busvoltage + (shuntvoltage / 1000.0);
      batt_current_A = (isnan(current_mA) || isinf(current_mA)) ? 0.0 : (current_mA / 1000.0);
      batt_power_W = (isnan(power_mW) || isinf(power_mW)) ? 0.0 : (power_mW / 1000.0);
      batt_soc = calculateBatterySoC(batt_voltage);
    }

    // Enforce Critical Load Hardware Pin Safety
    digitalWrite(RELAY1_PIN, RELAY_ON);
    relay1State = true;

    // Diagnostics
    Serial.printf("\n--- Telemetry Pulse #%d (Failures: %d) ---\n", sendCount + 1, failCount);
    Serial.printf("Inverter: %.1fV | %.2fA | %.1fW | PF:%.2f\n", p_voltage, p_current, p_power, p_pf);
    Serial.printf("Load:     %.1fV | %.2fA | %.1fW | Energy:%.2fWh\n", l_voltage, l_current, l_power, l_energy);
    Serial.printf("Battery:  %.2fV | %.2fA | %.1fW | SoC:%.1f%%\n", batt_voltage, batt_current_A, batt_power_W, batt_soc);
    Serial.printf("Relays:   R1=%s (Critical Locked) | R2=%s\n", relay1State ? "ON" : "OFF", relay2State ? "ON" : "OFF");

    sendSensorData();
    pollCommands();
  }

  // 3. LCD Screen Update Cycle
  if (now - lastDisplaySwitch >= LCD_INTERVAL) {
    lastDisplaySwitch = now;
    updateLCDScreen();
  }
}