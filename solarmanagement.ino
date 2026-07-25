#include <WiFi.h>
#include <HTTPClient.h>
#include <PZEM004Tv30.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_INA219.h>
#include <ArduinoJson.h>

// --- WiFi & Server Configurations ---
const char* WIFI_SSID = "THE METHOD ZONE";
const char* WIFI_PASS = "Chabu321+";
const char* SERVER_HOST = "solar-powered-stand-alone-management.onrender.com";
const int   SERVER_PORT = 443;
const char* API_DATA_URL    = "https://solar-powered-stand-alone-management.onrender.com/api/data";
const char* API_COMMANDS_URL = "https://solar-powered-stand-alone-management.onrender.com/api/commands";
const char* API_RELAY_ACK_URL = "https://solar-powered-stand-alone-management.onrender.com/api/relay/ack";

// --- Hardware Pin Definitions ---
const int RELAY1_PIN = 18;
const int RELAY2_PIN = 4;

// --- Active-LOW Relay Helpers ---
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

// --- Battery Specification ---
const float BATTERY_CAPACITY_AH = 70.0;

// --- Sensor Instantiations ---
LiquidCrystal_I2C lcd(0x27, 20, 4);
Adafruit_INA219 ina219;
HardwareSerial pzemLoadSerial(1);
PZEM004Tv30 pzemPrimary(Serial2, 25, 26);
PZEM004Tv30 pzemLoad(pzemLoadSerial, 16, 17);

// --- Global Metrics ---
float p_voltage = 0, p_current = 0, p_power = 0, p_energy = 0, p_pf = 0;
float l_voltage = 0, l_current = 0, l_power = 0, l_energy = 0, l_pf = 0;
float batt_voltage = 0, batt_current_mA = 0, batt_current_A = 0;
float batt_power_mW = 0, batt_power_W = 0, batt_soc = 0;

// --- Relay State Tracking ---
bool relay1State = true;
bool relay2State = true;

// --- Timing Variables ---
unsigned long lastSend = 0;
unsigned long lastCommandPoll = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastDisplaySwitch = 0;
int displayPage = 0;

// --- Send Interval Config ---
const unsigned long SEND_INTERVAL = 3000;
const unsigned long COMMAND_POLL_INTERVAL = 3000;
const unsigned long LCD_INTERVAL = 5000;

// ========== Battery SoC ==========
float calculateBatterySoC(float voltage) {
  if (voltage >= 12.70) return 100.0;
  if (voltage <= 10.50) return 0.0;
  float percentage = ((voltage - 10.50) / (12.70 - 10.50)) * 100.0;
  return constrain(percentage, 0.0, 100.0);
}

// ========== HTTP Helpers ==========
String httpPost(const char* url, String& jsonBody) {
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);
  int code = http.POST(jsonBody);
  String response = "";
  if (code > 0) {
    response = http.getString();
  } else {
    Serial.printf("HTTP POST error [%s]: %d\n", url, code);
  }
  http.end();
  return response;
}

String httpGet(const char* url) {
  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  String response = "";
  if (code > 0) {
    response = http.getString();
  } else {
    Serial.printf("HTTP GET error [%s]: %d\n", url, code);
  }
  http.end();
  return response;
}

// ========== Send Sensor Data ==========
void sendSensorData() {
  if (WiFi.status() != WL_CONNECTED) return;

  String json = "{";
  json += "\"inverter_voltage\":" + String(p_voltage, 1) + ",";
  json += "\"inverter_current\":" + String(p_current, 2) + ",";
  json += "\"inverter_power\":" + String(p_power, 1) + ",";
  json += "\"inverter_energy\":" + String(p_energy, 2) + ",";
  json += "\"inverter_pf\":" + String(p_pf, 2) + ",";
  json += "\"load_voltage\":" + String(l_voltage, 1) + ",";
  json += "\"load_current\":" + String(l_current, 2) + ",";
  json += "\"load_power\":" + String(l_power, 1) + ",";
  json += "\"load_energy\":" + String(l_energy, 2) + ",";
  json += "\"load_pf\":" + String(l_pf, 2) + ",";
  json += "\"battery_voltage\":" + String(batt_voltage, 2) + ",";
  json += "\"battery_current\":" + String(batt_current_A, 2) + ",";
  json += "\"battery_power\":" + String(batt_power_W, 2) + ",";
  json += "\"battery_soc\":" + String(batt_soc, 1) + ",";
  json += "\"battery_capacity_ah\":" + String(BATTERY_CAPACITY_AH, 0) + ",";
  json += "\"relay1_state\":\"" + String(relay1State ? "ON" : "OFF") + "\",";
  json += "\"relay2_state\":\"" + String(relay2State ? "ON" : "OFF") + "\",";
  json += "\"trip_state\":\"NOR\"";
  json += "}";

  String resp = httpPost(API_DATA_URL, json);
  if (resp.length() > 0) {
    Serial.printf("Data sent OK: %s\n", resp.c_str());
  }
}

// ========== Poll & Execute Commands ==========
void pollCommands() {
  if (WiFi.status() != WL_CONNECTED) return;

  String resp = httpGet(API_COMMANDS_URL);
  if (resp.length() == 0) return;

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, resp);
  if (err) {
    Serial.printf("JSON parse error: %s\n", err.c_str());
    return;
  }

  const char* cmd = doc["cmd"] | "none";
  if (strcmp(cmd, "none") == 0) return;

  if (strcmp(cmd, "relay") == 0) {
    int relay = doc["relay"] | 0;
    const char* state = doc["state"] | "OFF";

    bool turnOn = (strcmp(state, "ON") == 0);

    if (relay == 1) {
      digitalWrite(RELAY1_PIN, turnOn ? RELAY_ON : RELAY_OFF);
      relay1State = turnOn;
      Serial.printf("Relay 1 -> %s\n", state);
    } else if (relay == 2) {
      digitalWrite(RELAY2_PIN, turnOn ? RELAY_ON : RELAY_OFF);
      relay2State = turnOn;
      Serial.printf("Relay 2 -> %s\n", state);
    }

    // Send ACK
    String ack = "{";
    ack += "\"relay\":" + String(relay) + ",";
    ack += "\"state\":\"" + String(state) + "\",";
    ack += "\"status\":\"ok\"";
    ack += "}";
    httpPost(API_RELAY_ACK_URL, ack);
  }
}

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, RELAY_ON);
  digitalWrite(RELAY2_PIN, RELAY_ON);
  relay1State = true;
  relay2State = true;

  Serial.println("==============================================");
  Serial.println("SOLAR MICROGRID - ESP32 CONTROLLER");
  Serial.println("==============================================");

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("System Initializing");

  if (!ina219.begin()) {
    Serial.println("INA219 not found!");
  } else {
    ina219.setCalibration_32V_2A();
  }

  pzemLoadSerial.begin(9600, SERIAL_8N1, 16, 17);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi connected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi connection failed!");
  }
  lcd.clear();
}

// ========== Main Loop ==========
void loop() {
  unsigned long now = millis();

  // WiFi recovery
  if (now - lastWiFiCheck >= 10000) {
    lastWiFiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
    }
  }

  // Read sensors, send data, poll commands
  if (now - lastSend >= SEND_INTERVAL) {
    lastSend = now;

    // Read Primary PZEM (Inverter)
    p_voltage = pzemPrimary.voltage();
    p_current = pzemPrimary.current();
    p_power   = pzemPrimary.power();
    p_energy  = pzemPrimary.energy();
    p_pf      = pzemPrimary.pf();
    if (isnan(p_voltage)) { p_voltage = 0; p_current = 0; p_power = 0; p_energy = 0; p_pf = 0; }

    // Derived Load Metrics
    if (p_voltage > 2.0) {
      l_voltage = p_voltage - 2.0;
      l_current = p_current;
      l_pf = (p_pf > 0) ? p_pf : 0.95;
      l_power = l_voltage * l_current * l_pf;
      l_energy += (l_power * (SEND_INTERVAL / 1000.0)) / 3600000.0;
    } else {
      l_voltage = 0; l_current = 0; l_power = 0; l_pf = 0;
    }

    // Read INA219 (Battery)
    float busvoltage = ina219.getBusVoltage_V();
    float shuntvoltage = ina219.getShuntVoltage_mV();
    batt_voltage    = busvoltage + (shuntvoltage / 1000.0);
    batt_current_mA = ina219.getCurrent_mA();
    batt_current_A  = batt_current_mA / 1000.0;
    batt_power_mW   = ina219.getPower_mW();
    batt_power_W    = batt_power_mW / 1000.0;
    batt_soc        = calculateBatterySoC(batt_voltage);

    // Diagnostics
    Serial.printf("Inv: %.1fV %.1fW | Load: %.1fV %.1fW | Batt: %.2fV SoC:%.1f%% | R1:%s R2:%s\n",
                  p_voltage, p_power, l_voltage, l_power, batt_voltage, batt_soc,
                  relay1State ? "ON" : "OFF", relay2State ? "ON" : "OFF");

    // Send data to server
    sendSensorData();

    // Poll for commands from server
    pollCommands();
  }

  // LCD rotation
  if (now - lastDisplaySwitch >= LCD_INTERVAL) {
    lastDisplaySwitch = now;
    char rowBuffer[21];

    switch (displayPage) {
      case 0:
        lcd.setCursor(0, 0); lcd.print("--- INVERTER OUT ---");
        snprintf(rowBuffer, sizeof(rowBuffer), "Voltage: %5.1f V   ", p_voltage); lcd.setCursor(0, 1); lcd.print(rowBuffer);
        snprintf(rowBuffer, sizeof(rowBuffer), "Current: %5.2f A   ", p_current); lcd.setCursor(0, 2); lcd.print(rowBuffer);
        snprintf(rowBuffer, sizeof(rowBuffer), "Power: %4.1fW PF:%.2f", p_power, p_pf); lcd.setCursor(0, 3); lcd.print(rowBuffer);
        displayPage = 1;
        break;

      case 1:
        lcd.setCursor(0, 0); lcd.print("--- LOAD METRICS ---");
        snprintf(rowBuffer, sizeof(rowBuffer), "Voltage: %5.1f V   ", l_voltage); lcd.setCursor(0, 1); lcd.print(rowBuffer);
        snprintf(rowBuffer, sizeof(rowBuffer), "Current: %5.2f A   ", l_current); lcd.setCursor(0, 2); lcd.print(rowBuffer);
        snprintf(rowBuffer, sizeof(rowBuffer), "Power: %4.1fW PF:%.2f", l_power, l_pf); lcd.setCursor(0, 3); lcd.print(rowBuffer);
        displayPage = 2;
        break;

      case 2:
        lcd.setCursor(0, 0); lcd.print("--- BATT MONITOR ---");
        snprintf(rowBuffer, sizeof(rowBuffer), "Volt: %4.2fV  Cap:70Ah", batt_voltage); lcd.setCursor(0, 1); lcd.print(rowBuffer);
        snprintf(rowBuffer, sizeof(rowBuffer), "Curr: %5.2fA P:%4.1fW", batt_current_A, batt_power_W); lcd.setCursor(0, 2); lcd.print(rowBuffer);
        snprintf(rowBuffer, sizeof(rowBuffer), "SoC:  %5.1f %%  R1:%s R2:%s", batt_soc, relay1State ? "ON" : "OFF", relay2State ? "ON" : "OFF"); lcd.setCursor(0, 3); lcd.print(rowBuffer);
        displayPage = 0;
        break;
    }
  }
}
