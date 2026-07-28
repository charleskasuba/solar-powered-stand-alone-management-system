#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PZEM004Tv30.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_INA219.h>
#include <Preferences.h>

const char* WIFI_SSID = "THE METHOD ZONE";
const char* WIFI_PASS = "Chabu321+";
const char* SERVER_URL = "https://solar-powered-stand-alone-management.onrender.com";

const int RELAY1_PIN = 18;
const int RELAY2_PIN = 16;
#define RELAY_ON  LOW
#define RELAY_OFF HIGH
const float INVERTER_EFFICIENCY = 0.85;

Preferences prefs;
LiquidCrystal_I2C lcd(0x27, 20, 4);
Adafruit_INA219 ina219;
PZEM004Tv30 pzemPrimary(Serial2, 25, 26);
HardwareSerial pzemLoadSerial(1);
PZEM004Tv30 pzemLoad(pzemLoadSerial, 32, 33);

float p_voltage = 0, p_current = 0, p_power = 0, p_energy = 0, p_pf = 0;
float l_voltage = 0, l_current = 0, l_power = 0, l_energy = 0, l_pf = 0;
float batt_voltage = 12.60, batt_current_A = 0.0, batt_power_W = 0.0, batt_soc = 90.0;

bool relay1State = true, relay2State = true;

unsigned long lastSend = 0, lastWiFiCheck = 0, lastDisplaySwitch = 0;
int displayPage = 0, sendCount = 0, failCount = 0;
const unsigned long SEND_INTERVAL = 5000, LCD_INTERVAL = 4000;

float calculateBatterySoC(float voltage) {
  if (voltage >= 12.70) return 100.0;
  if (voltage <= 10.50) return 0.0;
  return constrain(((voltage - 10.50) / (12.70 - 10.50)) * 100.0, 0.0, 100.0);
}

void saveBatteryToFlash() {
  prefs.begin("batt", false);
  prefs.putFloat("v", batt_voltage);
  prefs.putFloat("soc", batt_soc);
  prefs.end();
}

void loadBatteryFromFlash() {
  prefs.begin("batt", true);
  batt_voltage = prefs.getFloat("v", 12.60);
  batt_soc = prefs.getFloat("soc", 90.0);
  prefs.end();
}

void sendSensorData() {
  if (WiFi.status() != WL_CONNECTED) return;
  String url = String(SERVER_URL) + "/api/update?";
  url += "iv=" + String(p_voltage, 1) + "&ic=" + String(p_current, 2);
  url += "&ip=" + String(p_power, 1) + "&ie=" + String(p_energy, 2);
  url += "&ipf=" + String(p_pf, 2);
  url += "&lv=" + String(l_voltage, 1) + "&lc=" + String(l_current, 2);
  url += "&lp=" + String(l_power, 1) + "&le=" + String(l_energy, 2);
  url += "&lpf=" + String(l_pf, 2);
  url += "&bv=" + String(batt_voltage, 2) + "&bc=" + String(batt_current_A, 2);
  url += "&bp=" + String(batt_power_W, 2) + "&bs=" + String(batt_soc, 1);
  url += "&r1=" + String(relay1State ? "ON" : "OFF");
  url += "&r2=" + String(relay2State ? "ON" : "OFF");

  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.begin(client, url); http.setTimeout(8000);
  int code = http.GET(); sendCount++;
  if (code > 0) { failCount = 0; }
  else { failCount++; Serial.printf("[FAIL] HTTP %d: %s\n", code, http.errorToString(code).c_str()); }
  http.end();
}

void pollCommands() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, String(SERVER_URL) + "/api/commands"); http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    String resp = http.getString();
    if (resp.indexOf("\"cmd\":\"relay\"") >= 0) {
      int relay = resp.substring(resp.indexOf("\"relay\":") + 8).toInt();
      bool turnOn = resp.indexOf("\"state\":\"ON\"") >= 0;
      if (relay == 1) {
        digitalWrite(RELAY1_PIN, RELAY_ON); relay1State = true;
        Serial.println(">> Relay 1 forced ON (critical)");
      } else if (relay == 2) {
        digitalWrite(RELAY2_PIN, turnOn ? RELAY_ON : RELAY_OFF);
        relay2State = turnOn;
        Serial.printf(">> Relay 2 -> %s\n", turnOn ? "ON" : "OFF");
      }
      // Send ACK back to server
      WiFiClientSecure ackClient; ackClient.setInsecure();
      HTTPClient ackHttp;
      String ackUrl = String(SERVER_URL) + "/api/relay/ack?relay=" + String(relay) + "&state=" + String(turnOn ? "ON" : "OFF");
      ackHttp.begin(ackClient, ackUrl); ackHttp.setTimeout(5000);
      ackHttp.GET(); ackHttp.end();
    }
  }
  http.end();
}

void updateLCDScreen() {
  char buf[21];
  lcd.clear();
  switch (displayPage) {
    case 0:
      lcd.setCursor(0, 0); lcd.print("== INVERTER OUTPUT ==");
      snprintf(buf, sizeof(buf), "V:%5.1fV  I:%5.2fA", p_voltage, p_current);
      lcd.setCursor(0, 1); lcd.print(buf);
      snprintf(buf, sizeof(buf), "P:%5.1fW  PF:%4.2f", p_power, p_pf);
      lcd.setCursor(0, 2); lcd.print(buf);
      snprintf(buf, sizeof(buf), "Sent:%d  Fail:%d", sendCount, failCount);
      lcd.setCursor(0, 3); lcd.print(buf);
      break;
    case 1:
      lcd.setCursor(0, 0); lcd.print("=== LOAD METRICS ===");
      snprintf(buf, sizeof(buf), "V:%5.1fV  I:%5.2fA", l_voltage, l_current);
      lcd.setCursor(0, 1); lcd.print(buf);
      snprintf(buf, sizeof(buf), "P:%5.1fW  PF:%4.2f", l_power, l_pf);
      lcd.setCursor(0, 2); lcd.print(buf);
      snprintf(buf, sizeof(buf), "Energy:%7.2fWh", l_energy);
      lcd.setCursor(0, 3); lcd.print(buf);
      break;
    case 2:
      lcd.setCursor(0, 0); lcd.print("== BATTERY STATUS ==");
      snprintf(buf, sizeof(buf), "V:%6.2fV  I:%5.2fA", batt_voltage, batt_current_A);
      lcd.setCursor(0, 1); lcd.print(buf);
      snprintf(buf, sizeof(buf), "P:%6.1fW  SoC:%5.1f%%", batt_power_W, batt_soc);
      lcd.setCursor(0, 2); lcd.print(buf);
      snprintf(buf, sizeof(buf), "R1:%s    R2:%s", relay1State ? "ON " : "OFF", relay2State ? "ON " : "OFF");
      lcd.setCursor(0, 3); lcd.print(buf);
      break;
    case 3:
      lcd.setCursor(0, 0); lcd.print("=== ENERGY TOTAL ===");
      snprintf(buf, sizeof(buf), "Produced:%8.2fWh", p_energy);
      lcd.setCursor(0, 1); lcd.print(buf);
      snprintf(buf, sizeof(buf), "Consumed:%8.2fWh", l_energy);
      lcd.setCursor(0, 2); lcd.print(buf);
      snprintf(buf, sizeof(buf), "SOC Balance:%5.1f%%", batt_soc);
      lcd.setCursor(0, 3); lcd.print(buf);
      break;
    case 4:
      lcd.setCursor(0, 0); lcd.print("=== SYSTEM INFO ====");
      lcd.setCursor(0, 1); lcd.print(WiFi.status() == WL_CONNECTED ? "WiFi: Connected" : "WiFi: DISCONNECTED");
      snprintf(buf, sizeof(buf), "IP: %s", WiFi.localIP().toString().c_str());
      lcd.setCursor(0, 2); lcd.print(buf);
      snprintf(buf, sizeof(buf), "Uptime: %d min", millis() / 60000);
      lcd.setCursor(0, 3); lcd.print(buf);
      break;
  }
  displayPage = (displayPage + 1) % 5;
}

void setup() {
  Serial.begin(115200); delay(1000);
  pinMode(RELAY1_PIN, OUTPUT); pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, RELAY_ON); digitalWrite(RELAY2_PIN, RELAY_ON);
  relay1State = relay2State = true;

  loadBatteryFromFlash();

  Serial.println("\n=== SOLAR MICROGRID ESP32 ===");
  Wire.begin(21, 22);
  lcd.init(); lcd.backlight(); lcd.clear();
  lcd.setCursor(0, 0); lcd.print("System Booting...");

  if (!ina219.begin()) { Serial.println("[WARN] INA219 not found"); }
  else { ina219.setCalibration_32V_2A(); Serial.println("[OK] INA219 ready"); }

  pzemLoadSerial.begin(9600, SERIAL_8N1, 32, 33);

  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  int att = 0;
  while (WiFi.status() != WL_CONNECTED && att < 20) { delay(500); att++; Serial.print("."); }

  lcd.clear();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[OK] IP: %s\n", WiFi.localIP().toString().c_str());
    lcd.setCursor(0, 0); lcd.print("WiFi Connected!");
    lcd.setCursor(0, 1); lcd.print(WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[FAIL] WiFi failed");
    lcd.setCursor(0, 0); lcd.print("WiFi Failed!");
    lcd.setCursor(0, 1); lcd.print("Offline Mode");
  }
  delay(2000); lcd.clear();
}

void loop() {
  unsigned long now = millis();

  if (now - lastWiFiCheck >= 15000) {
    lastWiFiCheck = now;
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  }

  if (now - lastSend >= SEND_INTERVAL) {
    lastSend = now;

    float temp_v = pzemPrimary.voltage();
    float temp_i = pzemPrimary.current();
    float temp_p = pzemPrimary.power();
    float temp_e = pzemPrimary.energy();
    float temp_pf = pzemPrimary.pf();

    if (isnan(temp_v) || temp_v < 0) {
      p_voltage = 0.0; p_current = 0.0; p_power = 0.0; p_pf = 0.0;
    } else {
      p_voltage = temp_v; p_current = temp_i; p_power = temp_p;
      p_energy = temp_e; p_pf = temp_pf;
    }

    float l_temp_v = pzemLoad.voltage();
    float l_temp_i = pzemLoad.current();
    float l_temp_p = pzemLoad.power();
    float l_temp_e = pzemLoad.energy();
    float l_temp_pf = pzemLoad.pf();

    if (!isnan(l_temp_v) && l_temp_v > 0) {
      l_voltage = l_temp_v; l_current = l_temp_i;
      l_power = l_temp_p; l_energy = l_temp_e; l_pf = l_temp_pf;
    } else if (p_voltage > 10.0) {
      l_voltage = p_voltage - 2.0; l_current = p_current;
      l_pf = (p_pf > 0) ? p_pf : 0.95;
      l_power = l_voltage * l_current * l_pf;
      l_energy += (l_power * (SEND_INTERVAL / 1000.0)) / 3600000.0;
    } else {
      l_voltage = 0; l_current = 0; l_power = 0; l_pf = 0;
    }

    float busvoltage = ina219.getBusVoltage_V();
    float shuntvoltage = ina219.getShuntVoltage_mV();
    float current_mA = ina219.getCurrent_mA();
    float power_mW = ina219.getPower_mW();

    if (!isnan(busvoltage) && !isinf(busvoltage) && busvoltage > 2.0) {
      batt_voltage = busvoltage + (shuntvoltage / 1000.0);
      batt_current_A = (isnan(current_mA) || isinf(current_mA)) ? 0.0 : (current_mA / 1000.0);
      batt_power_W = (isnan(power_mW) || isinf(power_mW)) ? 0.0 : (power_mW / 1000.0);
      batt_soc = calculateBatterySoC(batt_voltage);
      saveBatteryToFlash();
    } else if (p_voltage > 10.0) {
      batt_power_W = p_power / INVERTER_EFFICIENCY;
      batt_current_A = batt_voltage > 0 ? (batt_power_W / batt_voltage) : 0.0;
      batt_soc = calculateBatterySoC(batt_voltage);
    } else {
      batt_current_A = 0.0; batt_power_W = 0.0;
    }

    digitalWrite(RELAY1_PIN, RELAY_ON);
    relay1State = true;

    Serial.printf("\n--- Telemetry #%d ---\n", sendCount + 1);
    Serial.printf("Inverter: %.1fV %.2fA %.1fW PF:%.2f Energy:%.2fWh\n", p_voltage, p_current, p_power, p_pf, p_energy);
    Serial.printf("Load:     %.1fV %.2fA %.1fW Energy:%.2fWh\n", l_voltage, l_current, l_power, l_energy);
    Serial.printf("Battery:  %.2fV %.2fA %.1fW SoC:%.1f%%\n", batt_voltage, batt_current_A, batt_power_W, batt_soc);
    Serial.printf("Relays:   R1=%s R2=%s\n", relay1State ? "ON" : "OFF", relay2State ? "ON" : "OFF");

    sendSensorData();
    pollCommands();
  }

  if (now - lastDisplaySwitch >= LCD_INTERVAL) {
    lastDisplaySwitch = now;
    updateLCDScreen();
  }
}