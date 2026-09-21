// ── Config ─────────────────
const char* WIFI_SSID         = "FiberHome";
const char* WIFI_PASSWORD     = "Goldengate";
const char* AUTH_USER         = "admin";
const char* AUTH_PASSWORD     = "F4D47898";
const char* OTA_PASSWORD      = "F4D47898";
const char* NTP_SERVER        = "pool.ntp.org";
const long  GMT_OFFSET_SEC    = 18000;      // UTC+5 Karachi
const int   DAYLIGHT_OFFSET   = 0;          // No DST in Pakistan
const int   BOOT_GRACE_SEC    = 180;        // 3 min boot delay
const float COST_PER_KWH      = 56.0;       // PKR per kWh

// ── Pin Definitions ────────
// Coil relay module — active LOW (LOW=ON, HIGH=OFF)
// VCC must be 5V — do NOT use 3.3V or relay won't energize
const int RELAY_PINS[5] = {0, 11, 12, 13, 14}; // 1-based index (index 0 unused)

// Buttons (GPIOs 1-4, active-low)
const int BTN_PINS[5] = {0, 1, 2, 3, 4}; // 1-based index

// NTC Thermistor (10k NTC + 10k fixed resistor, GPIO 6 = ADC1_CH5)
// NTC_CONF: 0 = NTC to GND + pull-up to 3.3V, 1 = NTC to 3.3V + pull-down to GND
const int NTC_PIN = 6;
const float NTC_FIXED_R = 10000.0;    // Fixed resistor value (ohms)
const float NTC_NOMINAL_R = 10000.0;  // NTC resistance at 25°C
const float NTC_B_VALUE = 3950.0;     // NTC beta coefficient
const int NTC_CONF = 1;               // Set to 1 if temp reads way too high

// ── Libraries ──────────────
#include <WiFi.h>
#include <stdarg.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <time.h>
#include <Update.h>
#include <ArduinoJson.h>
#include "web_ui.h"

// ── Log Buffer ──────────────
#define MAX_LOG_LINES 80
#define MAX_LOG_LEN 120
struct LogBuffer {
  char lines[MAX_LOG_LINES][MAX_LOG_LEN];
  int head = 0;
  int tail = 0;
  int count = 0;

  void add(const char* msg) {
    strncpy(lines[head], msg, MAX_LOG_LEN - 1);
    lines[head][MAX_LOG_LEN - 1] = '\0';
    head = (head + 1) % MAX_LOG_LINES;
    if (count < MAX_LOG_LINES) count++;
    else tail = (tail + 1) % MAX_LOG_LINES;
  }

  String getAll() {
    String result = "[";
    for (int i = 0; i < count; i++) {
      int idx = (tail + i) % MAX_LOG_LINES;
      if (i > 0) result += ",";
      result += "\"";
      for (char* p = lines[idx]; *p; p++) {
        if (*p == '"') result += "\\\"";
        else if (*p == '\\') result += "\\\\";
        else if (*p == '\n') result += "\\n";
        else if (*p == '\r') continue;
        else result += *p;
      }
      result += "\"";
    }
    result += "]";
    return result;
  }

  void clear() {
    head = 0; tail = 0; count = 0;
  }
};

LogBuffer logBuffer;

void logMsg(const char* fmt, ...) {
  char buf[MAX_LOG_LEN];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, MAX_LOG_LEN, fmt, args);
  va_end(args);
  Serial.print(buf);
  logBuffer.add(buf);
}

// ── Globals + State ────────
WebServer server(80);
Preferences preferences;

// State Variables
bool lightStates[5] = {false, false, false, false, false};
String lightNames[5] = {"", "Front Green Belt", "Back Gate", "Side Wall", "Front Wall"};
float lightWatts[5] = {0.0, 20.0, 15.0, 25.0, 25.0};

// Scheduler Rules
struct ScheduleRule {
  bool days[7];      // 0=Sun, 1=Mon, ..., 6=Sat
  char on_time[6];   // "HH:MM"
  char off_time[6];  // "HH:MM"
  bool enabled;
};

#define MAX_RULES 4
ScheduleRule schedules[5][MAX_RULES];
int ruleCounts[5] = {0, 0, 0, 0, 0};

// Timing and System State
bool graceEnded = false;
bool timeSynced = false;
unsigned long lastGracePrintTime = 0;
unsigned long lastWifiCheckTime = 0;
unsigned long lastNtpSyncTime = 0;
int lastCheckedMinute = -1;

// Power Consumption Metrics
float hourlyEnergy[24] = {0.0};
float totalEnergySinceBoot = 0.0;
unsigned long lastEnergyUpdateTime = 0;
unsigned long secondsActive[5] = {0, 0, 0, 0, 0};
float energyConsumed[5] = {0.0, 0.0, 0.0, 0.0, 0.0};

// NTC Temperature State
float filteredTemperature = -1;
float tempCalibration = 0.0;

// Button Debounce States
int buttonState[5] = {HIGH, HIGH, HIGH, HIGH, HIGH};
int lastRawState[5] = {HIGH, HIGH, HIGH, HIGH, HIGH};
unsigned long lastDebounceTime[5] = {0, 0, 0, 0, 0};

// Forward Declarations
void setLightState(int id, bool state, const char* source);
void toggleLight(int id, const char* source);
void syncNTP();
void parseScheduleJson(int id, String jsonStr);
String serializeScheduleJson(int id);
void updatePowerMetrics();
bool hasActiveSchedule(int id);
void parseTimeStr(const char* timeStr, int& hour, int& min);
bool isTimeInActiveSchedule(int id, const struct tm& timeinfo);
void reconcileSchedules();

// ── Preferences / NVS ──────
void loadSettingsFromNVS() {
  preferences.begin("lights", false);
  
  if (!preferences.getBool("init", false)) {
    // Write defaults on very first boot
    preferences.putString("name1", "Front Green Belt");
    preferences.putString("name2", "Back Gate");
    preferences.putString("name3", "Side Wall");
    preferences.putString("name4", "Front Wall");
    preferences.putFloat("watt1", 20.0);
    preferences.putFloat("watt2", 15.0);
    preferences.putFloat("watt3", 25.0);
    preferences.putFloat("watt4", 25.0);
    preferences.putString("sched1", "[]");
    preferences.putString("sched2", "[]");
    preferences.putString("sched3", "[]");
    preferences.putString("sched4", "[]");
    preferences.putBool("state1", false);
    preferences.putBool("state2", false);
    preferences.putBool("state3", false);
    preferences.putBool("state4", false);
    preferences.putFloat("tempCal", 0.0);
    preferences.putBool("init", true);
  }
  
  for (int i = 1; i <= 4; i++) {
    lightNames[i] = preferences.getString(("name" + String(i)).c_str(), lightNames[i]);
    lightWatts[i] = preferences.getFloat(("watt" + String(i)).c_str(), lightWatts[i]);
    lightStates[i] = preferences.getBool(("state" + String(i)).c_str(), false);
    
    String schedJson = preferences.getString(("sched" + String(i)).c_str(), "[]");
    parseScheduleJson(i, schedJson);
  }
  
  tempCalibration = preferences.getFloat("tempCal", 0.0);
  
  preferences.end();
}

void saveLightStatesToNVS() {
  preferences.begin("lights", false);
  for (int i = 1; i <= 4; i++) {
    preferences.putBool(("state" + String(i)).c_str(), lightStates[i]);
  }
  preferences.end();
}

void parseScheduleJson(int id, String jsonStr) {
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, jsonStr);
  if (error) {
    ruleCounts[id] = 0;
    return;
  }
  
  JsonArray arr = doc.as<JsonArray>();
  int count = 0;
  for (JsonObject ruleObj : arr) {
    if (count >= MAX_RULES) break;
    ScheduleRule& rule = schedules[id][count];
    rule.enabled = ruleObj["enabled"] | false;
    
    const char* on_t = ruleObj["on_time"] | "18:00";
    const char* off_t = ruleObj["off_time"] | "06:00";
    strncpy(rule.on_time, on_t, sizeof(rule.on_time) - 1);
    strncpy(rule.off_time, off_t, sizeof(rule.off_time) - 1);
    
    JsonArray daysArr = ruleObj["days"];
    for (int d = 0; d < 7; d++) {
      rule.days[d] = daysArr[d] | false;
    }
    count++;
  }
  ruleCounts[id] = count;
}

String serializeScheduleJson(int id) {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < ruleCounts[id]; i++) {
    JsonObject ruleObj = arr.createNestedObject();
    ruleObj["enabled"] = schedules[id][i].enabled;
    ruleObj["on_time"] = schedules[id][i].on_time;
    ruleObj["off_time"] = schedules[id][i].off_time;
    JsonArray daysArr = ruleObj.createNestedArray("days");
    for (int d = 0; d < 7; d++) {
      daysArr.add(schedules[id][i].days[d]);
    }
  }
  String output;
  serializeJson(doc, output);
  return output;
}

// ── WiFi + NTP ─────────────
void setupWiFi() {
  logMsg("Connecting to WiFi: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startAttempt < 8000)) {
    delay(100);
    Serial.print(".");
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    logMsg("WiFi connected — IP: %s\n", WiFi.localIP().toString().c_str());
    syncNTP();
  } else {
    logMsg("WiFi connection timed out. Booting in offline mode.\n");
  }
  lastWifiCheckTime = millis();
}

void checkWifiConnection() {
  if (millis() - lastWifiCheckTime >= 10000) {
    lastWifiCheckTime = millis();
    if (WiFi.status() != WL_CONNECTED) {
      logMsg("WiFi connection lost. Reconnecting...\n");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }
}

void syncNTP() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET, NTP_SERVER);
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000)) {
    logMsg("Time synced: %02d:%02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    lastNtpSyncTime = millis();
    timeSynced = true;
    graceEnded = true; // Time verified: exit boot grace and reconcile immediately
    reconcileSchedules();
  } else {
    logMsg("NTP synchronization failed.\n");
  }
}

void checkNtpResync() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!timeSynced) {
      // Retry every 5 seconds until initial sync (handles slow router boot)
      if (millis() - lastNtpSyncTime >= 5000) {
        lastNtpSyncTime = millis();
        syncNTP();
      }
    } else if (millis() - lastNtpSyncTime >= 3600000) { // 1 hour regular resync
      syncNTP();
      logMsg("NTP re-synced\n");
    }
  }
}

// ── OTA ────────────────────
void setupArduinoOTA() {
  ArduinoOTA.setPort(3232);
  ArduinoOTA.setHostname("house-lights");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  
  ArduinoOTA.onStart([]() {
    logMsg("ArduinoOTA Update started.\n");
  });
  ArduinoOTA.onEnd([]() {
    logMsg("ArduinoOTA Update complete.\n");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    logMsg("OTA progress: %u%%\n", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    logMsg("ArduinoOTA Error[%u]\n", error);
  });
  ArduinoOTA.begin();
}

// ── Button Handling ────────
void handleButtons() {
  for (int i = 1; i <= 4; i++) {
    int pinVal = digitalRead(BTN_PINS[i]);
    if (pinVal != lastRawState[i]) {
      lastDebounceTime[i] = millis();
      lastRawState[i] = pinVal;
    }
    
    if ((millis() - lastDebounceTime[i]) > 30) {
      if (pinVal != buttonState[i]) {
        buttonState[i] = pinVal;
        if (buttonState[i] == LOW) {
          // Trigger toggle instantly on button confirmation
          toggleLight(i, "button");
        }
      }
    }
  }
}

// ── Light Control ──────────
void setLightState(int id, bool state, const char* source) {
  if (id < 1 || id > 4) return;
  lightStates[id] = state;
  digitalWrite(RELAY_PINS[id], state ? LOW : HIGH); // LOW = ON, HIGH = OFF
  logMsg("Light %d [%s] → %s (%s)\n", id, lightNames[id].c_str(), state ? "ON" : "OFF", source);
  saveLightStatesToNVS();
}

void toggleLight(int id, const char* source) {
  if (id < 1 || id > 4) return;
  setLightState(id, !lightStates[id], source);
}

// ── Schedule Checker ───────
bool hasActiveSchedule(int id) {
  if (id < 1 || id > 4) return false;
  for (int r = 0; r < ruleCounts[id]; r++) {
    if (schedules[id][r].enabled) return true;
  }
  return false;
}

void parseTimeStr(const char* timeStr, int& hour, int& min) {
  hour = 0; min = 0;
  sscanf(timeStr, "%d:%d", &hour, &min);
}

bool isTimeInActiveSchedule(int id, const struct tm& timeinfo) {
  int now = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int today = timeinfo.tm_wday; // 0=Sunday, ..., 6=Saturday
  int yesterday = (today + 6) % 7;

  for (int r = 0; r < ruleCounts[id]; r++) {
    const ScheduleRule& rule = schedules[id][r];
    if (!rule.enabled) continue;

    int on_h, on_m, off_h, off_m;
    parseTimeStr(rule.on_time, on_h, on_m);
    parseTimeStr(rule.off_time, off_h, off_m);

    int on = on_h * 60 + on_m;
    int off = off_h * 60 + off_m;

    if (on == off) continue; // 0-duration rule

    if (off > on) {
      // Same-day window (e.g. 08:00 to 17:00)
      if (now >= on && now < off && rule.days[today]) {
        return true;
      }
    } else {
      // Midnight-crossover window (e.g. 18:00 to 06:00 next day)
      // Evening portion on starting day
      if (now >= on && rule.days[today]) {
        return true;
      }
      // Morning portion running from previous day
      if (now < off && rule.days[yesterday]) {
        return true;
      }
    }
  }
  return false;
}

void reconcileSchedules() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 50)) return;

  logMsg("Reconciling schedules for %02d:%02d (Day %d)\n", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_wday);

  for (int id = 1; id <= 4; id++) {
    // Non-scheduled lights are purely manual — never altered by schedules
    if (!hasActiveSchedule(id)) continue;

    bool shouldBeOn = isTimeInActiveSchedule(id, timeinfo);
    if (lightStates[id] != shouldBeOn) {
      setLightState(id, shouldBeOn, "schedule");
      logMsg("Schedule reconciled: Light %d [%s] -> %s (current time %02d:%02d)\n",
        id, lightNames[id].c_str(), shouldBeOn ? "ON" : "OFF", timeinfo.tm_hour, timeinfo.tm_min);
    }
  }
}

void evaluateSchedules() {
  if (!graceEnded) return;
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 50)) return;
  
  int current_min = timeinfo.tm_min;
  if (current_min == lastCheckedMinute) return;
  lastCheckedMinute = current_min;
  
  int current_hour = timeinfo.tm_hour;
  int current_day = timeinfo.tm_wday; // 0=Sunday, ..., 6=Saturday
  
  logMsg("Schedule checked: %02d:%02d\n", current_hour, current_min);
  
  for (int id = 1; id <= 4; id++) {
    for (int r = 0; r < ruleCounts[id]; r++) {
      ScheduleRule& rule = schedules[id][r];
      if (!rule.enabled) continue;
      
      int on_h, on_m, off_h, off_m;
      parseTimeStr(rule.on_time, on_h, on_m);
      parseTimeStr(rule.off_time, off_h, off_m);
      
      // ON Match Check
      if (current_hour == on_h && current_min == on_m) {
        if (rule.days[current_day]) {
          setLightState(id, true, "schedule");
        }
      }
      
      // OFF Match Check
      if (current_hour == off_h && current_min == off_m) {
        int on_mins = on_h * 60 + on_m;
        int off_mins = off_h * 60 + off_m;
        bool isCrossover = off_mins < on_mins;
        int checkDay = isCrossover ? ((current_day + 6) % 7) : current_day;
        
        if (rule.days[checkDay]) {
          setLightState(id, false, "schedule");
        }
      }
    }
  }
}

// ── NTC Temperature ────────
float readTemperature() {
  long sum = 0;
  int valid = 0;
  for (int i = 0; i < 32; i++) {
    int v = analogRead(NTC_PIN);
    if (v > 0 && v < 4095) { sum += v; valid++; }
    delay(1);
  }
  if (valid == 0) return -1;
  int raw = sum / valid;

  float Vout = (raw / 4095.0) * 3.3;
  float R_ntc;

  if (NTC_CONF == 1) {
    // NTC to 3.3V, fixed pull-down to GND
    R_ntc = ((3.3 - Vout) * NTC_FIXED_R) / Vout;
  } else {
    // NTC to GND, fixed pull-up to 3.3V
    R_ntc = (Vout * NTC_FIXED_R) / (3.3 - Vout);
  }

  if (R_ntc <= 0) return -1;

  float steinhart = log(R_ntc / NTC_NOMINAL_R);
  steinhart /= NTC_B_VALUE;
  steinhart += 1.0 / 298.15;
  steinhart = 1.0 / steinhart;
  steinhart -= 273.15;
  return steinhart;
}

// ── Power Metrics ──────────
void updatePowerMetrics() {
  unsigned long now = millis();
  if (lastEnergyUpdateTime == 0) {
    lastEnergyUpdateTime = now;
    return;
  }
  
  float dt = (now - lastEnergyUpdateTime) / 1000.0;
  lastEnergyUpdateTime = now;
  
  struct tm timeinfo;
  int currentHour = 0;
  if (getLocalTime(&timeinfo)) {
    currentHour = timeinfo.tm_hour;
  }
  
  for (int i = 1; i <= 4; i++) {
    if (lightStates[i]) {
      secondsActive[i] += dt;
      float addedWh = (lightWatts[i] * dt) / 3600.0;
      energyConsumed[i] += addedWh;
      totalEnergySinceBoot += addedWh;
      
      if (currentHour >= 0 && currentHour < 24) {
        hourlyEnergy[currentHour] += addedWh;
      }
    }
  }
  
  // Clear new slot on hourly rollover to drop old data
  static int prevHour = -1;
  if (currentHour != prevHour) {
    if (prevHour != -1) {
      hourlyEnergy[currentHour] = 0.0;
    }
    prevHour = currentHour;
  }
}

// ── Web Server Routes ──────
bool checkAuth() {
  if (!server.authenticate(AUTH_USER, AUTH_PASSWORD)) {
    server.requestAuthentication(BASIC_AUTH, "House Lights");
    return false;
  }
  return true;
}

void sendCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
}

void setupWebServer() {
  // OPTIONS preflights
  server.on("/status", HTTP_OPTIONS, []() { sendCORSHeaders(); server.send(200, "text/plain", ""); });
  server.on("/schedules", HTTP_OPTIONS, []() { sendCORSHeaders(); server.send(200, "text/plain", ""); });
  server.on("/settings", HTTP_OPTIONS, []() { sendCORSHeaders(); server.send(200, "text/plain", ""); });
  server.on("/ota", HTTP_OPTIONS, []() { sendCORSHeaders(); server.send(200, "text/plain", ""); });
  server.on("/logs", HTTP_OPTIONS, []() { sendCORSHeaders(); server.send(200, "text/plain", ""); });

  // Web GUI
  server.on("/", HTTP_GET, []() {
    if (!checkAuth()) return;
    sendCORSHeaders();
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (const char*)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
  });

  // /ping
  server.on("/ping", HTTP_GET, []() {
    sendCORSHeaders();
    server.send(200, "text/plain", "pong");
  });

  // /logs
  server.on("/logs", HTTP_GET, []() {
    if (!checkAuth()) return;
    sendCORSHeaders();
    String json = logBuffer.getAll();
    server.send(200, "application/json", json);
  });

  // /logs DELETE (clear logs)
  server.on("/logs", HTTP_DELETE, []() {
    if (!checkAuth()) return;
    sendCORSHeaders();
    logBuffer.clear();
    logMsg("Logs cleared via web UI\n");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // /status
  server.on("/status", HTTP_GET, []() {
    if (!checkAuth()) return;
    updatePowerMetrics();
    
    DynamicJsonDocument doc(4096);
    struct tm timeinfo;
    char timeStr[6] = "00:00";
    int day = 0;
    if (getLocalTime(&timeinfo)) {
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
      day = timeinfo.tm_wday;
    }
    
    doc["time"] = timeStr;
    doc["day"] = day;
    doc["uptime"] = millis() / 1000;
    
    long graceRemaining = 0;
    long elapsed = millis() / 1000;
    if (elapsed < BOOT_GRACE_SEC) {
      graceRemaining = BOOT_GRACE_SEC - elapsed;
    }
    doc["graceRemaining"] = graceRemaining;
    doc["freeHeap"] = ESP.getFreeHeap();

    float rawTemp = readTemperature();
    if (rawTemp > -1) {
      if (filteredTemperature < 0) filteredTemperature = rawTemp;
      else filteredTemperature += (rawTemp - filteredTemperature) * 0.2;
    }
    doc["temperature"] = filteredTemperature + tempCalibration;
    
    JsonObject lightsObj = doc.createNestedObject("lights");
    for (int i = 1; i <= 4; i++) {
      JsonObject lObj = lightsObj.createNestedObject(String(i));
      lObj["name"] = lightNames[i];
      lObj["state"] = lightStates[i];
      lObj["watt"] = lightWatts[i];
      lObj["hoursOn"] = (float)secondsActive[i] / 3600.0;
      lObj["whToday"] = energyConsumed[i];
    }
    
    JsonArray chartArr = doc.createNestedArray("chart");
    for (int i = 0; i < 24; i++) {
      chartArr.add(hourlyEnergy[i]);
    }
    
    String response;
    serializeJson(doc, response);
    
    sendCORSHeaders();
    server.send(200, "application/json", response);
  });

  // /schedules GET
  server.on("/schedules", HTTP_GET, []() {
    if (!checkAuth()) return;
    DynamicJsonDocument doc(4096);
    for (int i = 1; i <= 4; i++) {
      JsonArray arr = doc.createNestedArray("sched" + String(i));
      for (int r = 0; r < ruleCounts[i]; r++) {
        JsonObject ruleObj = arr.createNestedObject();
        ruleObj["enabled"] = schedules[i][r].enabled;
        ruleObj["on_time"] = schedules[i][r].on_time;
        ruleObj["off_time"] = schedules[i][r].off_time;
        JsonArray daysArr = ruleObj.createNestedArray("days");
        for (int d = 0; d < 7; d++) {
          daysArr.add(schedules[i][r].days[d]);
        }
      }
    }
    String response;
    serializeJson(doc, response);
    sendCORSHeaders();
    server.send(200, "application/json", response);
  });

  // /schedules POST
  server.on("/schedules", HTTP_POST, []() {
    if (!checkAuth()) return;
    if (!server.hasArg("plain")) {
      sendCORSHeaders();
      server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Body missing\"}");
      return;
    }
    
    String body = server.arg("plain");
    DynamicJsonDocument doc(8192);
    DeserializationError error = deserializeJson(doc, body);
    if (error) {
      sendCORSHeaders();
      server.send(400, "application/json", "{\"ok\":false,\"msg\":\"JSON error\"}");
      return;
    }
    
    preferences.begin("lights", false);
    for (int i = 1; i <= 4; i++) {
      String key = "sched" + String(i);
      if (doc.containsKey(key)) {
        JsonArray arr = doc[key];
        String output;
        serializeJson(arr, output);
        preferences.putString(key.c_str(), output);
        parseScheduleJson(i, output);
      }
    }
    preferences.end();
    if (timeSynced) {
      reconcileSchedules();
    }
    
    sendCORSHeaders();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // /settings GET
  server.on("/settings", HTTP_GET, []() {
    if (!checkAuth()) return;
    DynamicJsonDocument doc(1024);
    JsonArray names = doc.createNestedArray("names");
    JsonArray watts = doc.createNestedArray("watts");
    for (int i = 1; i <= 4; i++) {
      names.add(lightNames[i]);
      watts.add(lightWatts[i]);
    }
    doc["tempCal"] = tempCalibration;
    String response;
    serializeJson(doc, response);
    sendCORSHeaders();
    server.send(200, "application/json", response);
  });

  // /settings POST
  server.on("/settings", HTTP_POST, []() {
    if (!checkAuth()) return;
    if (!server.hasArg("plain")) {
      sendCORSHeaders();
      server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Body missing\"}");
      return;
    }
    
    String body = server.arg("plain");
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, body);
    if (error) {
      sendCORSHeaders();
      server.send(400, "application/json", "{\"ok\":false,\"msg\":\"JSON error\"}");
      return;
    }
    
    preferences.begin("lights", false);
    if (doc.containsKey("names")) {
      JsonArray names = doc["names"];
      for (int i = 0; i < 4; i++) {
        int id = i + 1;
        String newName = names[i] | "";
        if (newName.length() > 0 && newName.length() <= 32) {
          lightNames[id] = newName;
          preferences.putString(("name" + String(id)).c_str(), newName);
          logMsg("Settings saved: name%d=%s\n", id, newName.c_str());
        }
      }
    }
    
    if (doc.containsKey("watts")) {
      JsonArray watts = doc["watts"];
      for (int i = 0; i < 4; i++) {
        int id = i + 1;
        float newWatt = watts[i] | 25.0;
        if (newWatt > 0.0) {
          lightWatts[id] = newWatt;
          preferences.putFloat(("watt" + String(id)).c_str(), newWatt);
          logMsg("Settings saved: watt%d=%.2f\n", id, newWatt);
        }
      }
    }
    
    if (doc.containsKey("tempCal")) {
      float newCal = doc["tempCal"] | 0.0;
      if (newCal >= -100.0 && newCal <= 100.0) {
        tempCalibration = newCal;
        preferences.putFloat("tempCal", newCal);
        logMsg("Settings saved: tempCal=%.1f\n", newCal);
      }
    }
    preferences.end();
    
    sendCORSHeaders();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // /ota POST
  server.on("/ota", HTTP_POST, []() {
    if (!server.authenticate(AUTH_USER, AUTH_PASSWORD)) {
      return;
    }
    sendCORSHeaders();
    server.sendHeader("Connection", "close");
    if (Update.hasError()) {
      server.send(500, "application/json", "{\"status\":\"error\",\"msg\":\"Update Failed\"}");
    } else {
      server.send(200, "application/json", "{\"status\":\"ok\"}");
      delay(1000);
      ESP.restart();
    }
  }, []() {
    if (!server.authenticate(AUTH_USER, AUTH_PASSWORD)) {
      server.requestAuthentication(BASIC_AUTH, "House Lights");
      return;
    }
    
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      logMsg("OTA upload started: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
      if (Update.size() > 0) {
        int progress = (Update.progress() * 100) / Update.size();
        static int lastProgress = -1;
        if (progress != lastProgress) {
          logMsg("OTA progress: %d%%\n", progress);
          lastProgress = progress;
        }
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        logMsg("OTA update success: %u bytes. Rebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  // Wildcard dynamic routes (/light/{id}/{action})
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      sendCORSHeaders();
      server.send(200, "text/plain", "");
      return;
    }
    
    String uri = server.uri();
    if (uri.startsWith("/light/")) {
      if (!checkAuth()) return;
      int slash2 = uri.indexOf('/', 7);
      if (slash2 != -1) {
        int id = uri.substring(7, slash2).toInt();
        String action = uri.substring(slash2 + 1);
        if (id >= 1 && id <= 4) {
          if (action == "on") {
            setLightState(id, true, "web");
            sendCORSHeaders();
            server.send(200, "application/json", "{\"ok\":true}");
            return;
          } else if (action == "off") {
            setLightState(id, false, "web");
            sendCORSHeaders();
            server.send(200, "application/json", "{\"ok\":true}");
            return;
          } else if (action == "toggle") {
            toggleLight(id, "web");
            sendCORSHeaders();
            String resp = "{\"ok\":true,\"state\":" + String(lightStates[id] ? "true" : "false") + "}";
            server.send(200, "application/json", resp);
            return;
          }
        }
      }
    }
    
    sendCORSHeaders();
    server.send(404, "text/plain", "Not Found");
  });
}

// ── Setup ──────────────────
void setup() {
  Serial.begin(115200);
  delay(100);
  
  // Pin setup
  logMsg("System booting — House Lighting System v1.0\n");
  for (int i = 1; i <= 4; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], HIGH);  // HIGH = relay OFF = light OFF
    
    pinMode(BTN_PINS[i], INPUT_PULLUP);
  }
  
  // Load configuration
  loadSettingsFromNVS();
  
  // Restore relay outputs:
  // - Scheduled lights: Hold relay OFF until WiFi and time sync to avoid daylight instant-on.
  // - Non-scheduled (manual) lights: Restore previous state immediately from NVS.
  for (int i = 1; i <= 4; i++) {
    if (hasActiveSchedule(i)) {
      digitalWrite(RELAY_PINS[i], HIGH); // Relay OFF (Light OFF)
      lightStates[i] = false;            // Held OFF pending NTP schedule reconciliation
      logMsg("Light %d [%s] is scheduled — holding OFF until time sync\n", i, lightNames[i].c_str());
    } else {
      digitalWrite(RELAY_PINS[i], lightStates[i] ? LOW : HIGH);
      logMsg("Restoring manual Light %d [%s] to %s\n", i, lightNames[i].c_str(), lightStates[i] ? "ON" : "OFF");
    }
  }
  
  // NTC ADC setup
  analogReadResolution(12);
  analogSetPinAttenuation(NTC_PIN, ADC_11db);

  // WiFi connection (non-blocking for 8s)
  setupWiFi();
  
  // Setup OTA handlers
  setupArduinoOTA();
  
  // Web Server
  setupWebServer();
  server.begin();
  
  logMsg("Setup finished. System active.\n");
}

// ── Loop ───────────────────
void loop() {
  // Handle local physical buttons (never blocked by grace period, runs offline)
  handleButtons();
  
  // Keep alive networks & servers
  checkWifiConnection();
  checkNtpResync();
  server.handleClient();
  ArduinoOTA.handle();
  
  // Power tracking accumulator
  updatePowerMetrics();
  
  // Boot grace countdown timer
  unsigned long elapsed = millis() / 1000;
  if (!graceEnded) {
    if (elapsed >= (unsigned long)BOOT_GRACE_SEC) {
      graceEnded = true;
      logMsg("Boot grace ended — schedules active\n");
      reconcileSchedules();
    } else {
      if (millis() - lastGracePrintTime >= 30000) {
        lastGracePrintTime = millis();
        logMsg("Boot grace: %ds remaining\n", (int)(BOOT_GRACE_SEC - elapsed));
      }
    }
  }
  
  // Evaluates timer schedule rules
  evaluateSchedules();
  
  // Gentle yield to prevent watchdog resets
  yield();
}
