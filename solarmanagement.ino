#include <WiFi.h>
#include <HTTPClient.h>
#include <PZEM004Tv30.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_INA219.h>

// --- WiFi & Server Configurations ---
const char* WIFI_SSID = "THE METHOD ZONE";
const char* WIFI_PASS = "Chabu321+";
const char* SERVER_URL = "https://smartmeter-isps.onrender.com/api/data";

// --- Hardware Pin Definitions ---
const int RELAY1_PIN = 18; // Relay 1 (G18)
const int RELAY2_PIN = 4;  // Relay 2 (G4)

// --- Active-LOW Relay Helpers (Standard Relay Boards) ---
#define RELAY_ON  LOW   // Driving PIN LOW energizes the relay
#define RELAY_OFF HIGH  // Driving PIN HIGH turns off the relay

// --- Battery Specification ---
const float BATTERY_CAPACITY_AH = 70.0; 

// --- Sensor Instantiations ---
LiquidCrystal_I2C lcd(0x27, 20, 4); 
Adafruit_INA219 ina219;

// Instantiate HardwareSerial for ESP32
HardwareSerial pzemLoadSerial(1); // Use UART1 (Serial1)

// Primary PZEM (Inverter Output Side) via Hardware Serial 2 (Pins 25 RX, 26 TX)
PZEM004Tv30 pzemPrimary(Serial2, 25, 26);

// Load PZEM (Load Side) via Hardware Serial 1 (Pins 16 RX, 17 TX)
PZEM004Tv30 pzemLoad(pzemLoadSerial, 16, 17);

// --- Global Metrics ---
// Inverter Side (PZEM Primary)
float p_voltage = 0, p_current = 0, p_power = 0, p_energy = 0, p_pf = 0;

// Load Side (PZEM Secondary - Derived via Offset)
float l_voltage = 0, l_current = 0, l_power = 0, l_energy = 0, l_pf = 0;

// Battery (INA219)
float batt_voltage = 0, batt_current_mA = 0, batt_current_A = 0;
float batt_power_mW = 0, batt_power_W = 0, batt_soc = 0;

// --- Timing Variables ---
unsigned long lastSend = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastDisplaySwitch = 0;
int displayPage = 0; 

// Helper function to calculate Lead-Acid / LiFePO4 12V Battery Percentage
float calculateBatterySoC(float voltage) {
  if (voltage >= 12.70) return 100.0;
  if (voltage <= 10.50) return 0.0;
  float percentage = ((voltage - 10.50) / (12.70 - 10.50)) * 100.0;
  return constrain(percentage, 0.0, 100.0);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n==============================================");
  Serial.println("INVERTER & LOAD MANAGEMENT SYSTEM ACTIVE");
  Serial.println("==============================================");

  // 1. Initialize Relays to ON (Active-LOW: LOW turns relay ON)
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, RELAY_ON); 
  digitalWrite(RELAY2_PIN, RELAY_ON);

  // Initialize I2C (SDA = 21, SCL = 22)
  Wire.begin(21, 22); 

  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("System Initializing");

  // Initialize INA219
  if (!ina219.begin()) {
    Serial.println("Failed to find INA219 chip!");
  } else {
    ina219.setCalibration_32V_2A();
  }

  // Initialize Hardware Serial 1 for Load PZEM (Pins 16 RX, 17 TX)
  pzemLoadSerial.begin(9600, SERIAL_8N1, 16, 17);

  // Connect to WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 15) {
    delay(500);
    attempts++;
  }
  
  lcd.clear();
}

void loop() {
  unsigned long now = millis();

  // Ensure Relays remain engaged (ON)
  digitalWrite(RELAY1_PIN, RELAY_ON);
  digitalWrite(RELAY2_PIN, RELAY_ON);

  // --- WIFI RECOVERY ---
  if (now - lastWiFiCheck >= 10000) {
    lastWiFiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
    }
  }

  // --- DATA SAMPLING & API DELIVERY (Every 3 seconds) ---
  if (now - lastSend >= 3000) {
    lastSend = now;

    // 1. Read Primary PZEM (Inverter Side)
    p_voltage = pzemPrimary.voltage();
    p_current = pzemPrimary.current();
    p_power   = pzemPrimary.power();
    p_energy  = pzemPrimary.energy();
    p_pf      = pzemPrimary.pf();
    if (isnan(p_voltage)) { p_voltage = 0; p_current = 0; p_power = 0; p_energy = 0; p_pf = 0; }

    // 2. LOAD SIDE (Derived Logic)
    if (p_voltage > 2.0) {
      l_voltage = p_voltage - 2.0;
      l_current = p_current;
      l_pf      = (p_pf > 0) ? p_pf : 0.95;
      l_power   = l_voltage * l_current * l_pf;
      l_energy += (l_power * 3.0) / 3600000.0; 
    } else {
      l_voltage = 0.0;
      l_current = 0.0;
      l_power   = 0.0;
      l_pf      = 0.0;
    }

    // 3. Read INA219 (Battery Side)
    float busvoltage = ina219.getBusVoltage_V();
    float shuntvoltage = ina219.getShuntVoltage_mV();
    batt_voltage    = busvoltage + (shuntvoltage / 1000.0);
    batt_current_mA = ina219.getCurrent_mA();
    batt_current_A  = batt_current_mA / 1000.0;
    batt_power_mW   = ina219.getPower_mW();
    batt_power_W    = batt_power_mW / 1000.0;
    batt_soc        = calculateBatterySoC(batt_voltage);

    // --- SERIAL DIAGNOSTIC REPORT ---
    Serial.println("\n--- SYSTEM DIAGNOSTIC REPORT ---");
    Serial.printf("Relay Status           : RELAY1=ON (LOW) | RELAY2=ON (LOW)\n");
    Serial.printf("Inverter Out (PZEM 1)  : V=%.1fV | I=%.2fA | P=%.1fW | PF=%.2f\n", p_voltage, p_current, p_power, p_pf);
    Serial.printf("Load Side    (Derived) : V=%.1fV | I=%.2fA | P=%.1fW | PF=%.2f\n", l_voltage, l_current, l_power, l_pf);
    Serial.printf("Battery      (INA219)  : V=%.2fV | I=%.2fA | P=%.2fW | SoC=%.1f%%\n", batt_voltage, batt_current_A, batt_power_W, batt_soc);
    Serial.println("-------------------------------------------------");

    // --- SEND API PAYLOAD ---
    if (WiFi.status() == WL_CONNECTED) {
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
      json += "\"relay1_state\":\"ON\",";
      json += "\"relay2_state\":\"ON\"";
      json += "}";

      HTTPClient http;
      http.begin(SERVER_URL);
      http.addHeader("Content-Type", "application/json");
      int responseCode = http.POST(json);
      if (responseCode > 0) {
        Serial.printf("📤 API Response [%d]: %s\n", responseCode, http.getString().c_str());
      } else {
        Serial.printf("📤 API Error: %d\n", responseCode);
      }
      http.end();
    }
  }

  // --- LCD ROTATION LOGIC (Every 5 seconds) ---
  if (now - lastDisplaySwitch >= 5000) {
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
        snprintf(rowBuffer, sizeof(rowBuffer), "SoC:  %5.1f %%       ", batt_soc); lcd.setCursor(0, 3); lcd.print(rowBuffer);
        displayPage = 0;  
        break;
    }
  }
}