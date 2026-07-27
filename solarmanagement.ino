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

// --- Auto-Load Shedding Config ---
const float SOC_WARNING_THRESHOLD = 30.0;
const float SOC_CRITICAL_THRESHOLD = 20.0;
bool shedRelay2 = false;

// --- Sensors & Hardware ---
// Make sure your I2C address matches (0x27 or 0x3F are common)
LiquidCrystal_I2C lcd(0x27, 20, 4); 
Adafruit_INA219 ina219;

// Serial2 for Inverter/Primary PZEM: RX=25, TX=26
PZEM004Tv30 pzemPrimary(Serial2, 25, 26);

// Serial1 for Dedicated Load PZEM (if attached): RX=16, TX=17
HardwareSerial pzemLoadSerial(1);
PZEM004Tv30 pzemLoad(pzemLoadSerial, 16, 17);

// --- Metrics Variables ---
float p_voltage = 0, p_current = 0, p_power = 0, p_energy = 0, p_pf = 0;
float l_voltage = 0, l_current = 0, l_power = 0, l_energy = 0, l_pf = 0;
float batt_voltage = 0, batt_current_A = 0, batt_power_W = 0, batt_soc = 0;

// --- Relay State ---
bool relay1State = true;
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

// Calculate State of Charge based on Lead-Acid 12V voltage curve
float calculateBatterySoC(float voltage) {
  if (voltage >= 12.70) return 100.0;
  if (voltage <= 10.50) return 0.0;
  return constrain(((voltage - 10.50) / (12.70 - 10.50)) * 100.0, 0.0, 100.0);
}

// ========== Auto-Load Shedding ==========
void handleLoadShedding() {
  if (!shedRelay2 && batt_soc < SOC_CRITICAL_THRESHOLD) {
    digitalWrite(RELAY2_PIN, RELAY_OFF);
    shedRelay2 = true;
    relay2State = false;
    Serial.printf("[SHEDDING] Relay 2 OFF - Battery critical (SoC: %.1f%% < %.1f%%)\n", batt_soc, SOC_CRITICAL_THRESHOLD);
  } else if (shedRelay2 && batt_soc > (SOC_CRITICAL_THRESHOLD + 5.0)) {
    digitalWrite(RELAY2_PIN, RELAY_ON);
    shedRelay2 = false;
    relay2State = true;
    Serial.printf("[SHEDDING] Relay 2 ON - Battery recovered (SoC: %.1f%% > %.1f%%)\n", batt_soc, SOC_CRITICAL_THRESHOLD + 5.0);
  }
}

// ========== Send Sensor Data via HTTP GET ==========
void sendSensorData() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] WiFi not connected, skipping send");
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
  url += "&sh=" + String(shedRelay2 ? "1" : "0");

  WiFiClientSecure client;
  client.setInsecure(); // Skip SSL certificate verification

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

// ========== Poll Commands ==========
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
        digitalWrite(RELAY1_PIN, turnOn ? RELAY_ON : RELAY_OFF);
        relay1State = turnOn;
        Serial.printf(">> Relay 1 commanded -> %s\n", turnOn ? "ON" : "OFF");
      } else if (relay == 2) {
        digitalWrite(RELAY2_PIN, turnOn ? RELAY_ON : RELAY_OFF);
        relay2State = turnOn;
        if (!turnOn) {
          shedRelay2 = false; // Manual override disables active load shed lock
        }
        Serial.printf(">> Relay 2 commanded -> %s\n", turnOn ? "ON" : "OFF");
      }
    }
  }
  http.end();
}

// ========== LCD Refresh Screen Function ==========
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
      lcd.setCursor(0, 0); lcd.print("--- BATTERY METRICS -");
      snprintf(buf, sizeof(buf), "V:%5.2fV  I:%5.2fA", batt_voltage, batt_current_A); 
      lcd.setCursor(0, 1); lcd.print(buf);
      snprintf(buf, sizeof(buf), "P:%5.1fW  SoC:%4.1f%%", batt_power_W, batt_soc); 
      lcd.setCursor(0, 2); lcd.print(buf);
      snprintf(buf, sizeof(buf), "R1:%s R2:%s Shed:%s", relay1State ? "ON" : "OFF", relay2State ? "ON" : "OFF", shedRelay2 ? "1" : "0"); 
      lcd.setCursor(0, 3); lcd.print(buf);
      displayPage = 0;
      break;
  }
}

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize Relay GPIOs
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, RELAY_ON);
  digitalWrite(RELAY2_PIN, RELAY_ON);
  relay1State = true;
  relay2State = true;

  Serial.println("\n==============================================");
  Serial.println("  SOLAR MICROGRID - ESP32 CONTROLLER");
  Serial.println("==============================================");

  // Initialize I2C and LCD Display
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("System Booting...");
  lcd.setCursor(0, 1);
  lcd.print("Initializing WiFi...");

  // Initialize INA219
  if (!ina219.begin()) {
    Serial.println("[WARN] INA219 not found on I2C bus!");
  } else {
    ina219.setCalibration_32V_2A();
    Serial.println("[OK] INA219 initialized!");
  }

  // Initialize Hardware Serials for PZEMs
  pzemLoadSerial.begin(9600, SERIAL_8N1, 16, 17);
  Serial.println("[OK] PZEM Serial Ports initialized!");

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
    lcd.print("WiFi Connection");
    lcd.setCursor(0, 1);
    lcd.print("FAILED! Running offline");
  }
  delay(2000);
  lcd.clear();
}

// ========== Main Loop ==========
void loop() {
  unsigned long now = millis();

  // 1. Non-blocking WiFi Recovery
  if (now - lastWiFiCheck >= 15000) {
    lastWiFiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Reconnecting...");
      WiFi.reconnect();
    }
  }

  // 2. Read Sensors, Shed Load & Transmit Data
  if (now - lastSend >= SEND_INTERVAL) {
    lastSend = now;

    // --- Read Inverter PZEM (Primary) ---
    float temp_v = pzemPrimary.voltage();
    float temp_i = pzemPrimary.current();
    float temp_p = pzemPrimary.power();
    float temp_e = pzemPrimary.energy();
    float temp_pf = pzemPrimary.pf();

    if (isnan(temp_v) || temp_v < 0) {
      p_voltage = 0.0; p_current = 0.0; p_power = 0.0; p_pf = 0.0;
      Serial.println("[PZEM] Primary read failed or AC disconnected.");
    } else {
      p_voltage = temp_v;
      p_current = temp_i;
      p_power   = temp_p;
      p_energy  = temp_e;
      p_pf      = temp_pf;
    }

    // --- Calculated / Derived Load Metrics ---
    if (p_voltage > 10.0) { // Check if inverter output is active AC
      l_voltage = p_voltage - 2.0; // Simulated voltage drop
      l_current = p_current;
      l_pf = (p_pf > 0) ? p_pf : 0.95;
      l_power = l_voltage * l_current * l_pf;
      l_energy += (l_power * (SEND_INTERVAL / 1000.0)) / 3600000.0; // Accumulate Wh
    } else {
      l_voltage = 0; l_current = 0; l_power = 0; l_pf = 0;
    }

    // --- Read INA219 Battery Data ---
    float busvoltage = ina219.getBusVoltage_V();
    float shuntvoltage = ina219.getShuntVoltage_mV();
    float current_mA = ina219.getCurrent_mA();
    float power_mW = ina219.getPower_mW();

    if (isnan(busvoltage) || busvoltage <= 0.0) {
      batt_voltage = 0.0;
      batt_current_A = 0.0;
      batt_power_W = 0.0;
      batt_soc = 0.0;
    } else {
      batt_voltage = busvoltage + (shuntvoltage / 1000.0);
      batt_current_A = isnan(current_mA) ? 0.0 : (current_mA / 1000.0);
      batt_power_W = isnan(power_mW) ? 0.0 : (power_mW / 1000.0);
      batt_soc = calculateBatterySoC(batt_voltage);
    }

    // --- Console Diagnostic Print ---
    Serial.printf("\n--- Reading #%d (Fails: %d) ---\n", sendCount + 1, failCount);
    Serial.printf("Inv:  %.1fV | %.2fA | %.1fW | PF:%.2f\n", p_voltage, p_current, p_power, p_pf);
    Serial.printf("Load: %.1fV | %.2fA | %.1fW\n", l_voltage, l_current, l_power);
    Serial.printf("Batt: %.2fV | %.2fA | %.1fW | SoC:%.1f%%\n", batt_voltage, batt_current_A, batt_power_W, batt_soc);
    Serial.printf("Relays: R1=%s R2=%s | Shedding: %s\n", 
                  relay1State ? "ON" : "OFF", 
                  relay2State ? "ON" : "OFF", 
                  shedRelay2 ? "ACTIVE" : "OFF");

    // Execute Load Shedding Algorithm & Send Data
    handleLoadShedding();
    sendSensorData();
    pollCommands();
  }

  // 3. Update LCD Screen Non-blocking
  if (now - lastDisplaySwitch >= LCD_INTERVAL) {
    lastDisplaySwitch = now;
    updateLCDScreen();
  }
}