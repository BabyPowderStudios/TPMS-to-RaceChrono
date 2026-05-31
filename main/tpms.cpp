#include "tpms.h"

#include <Preferences.h>
#include <RadioLib.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFi.h>

#if defined(ESP32)
#define TPMS_ISR_ATTR IRAM_ATTR
#elif defined(ESP8266)
#define TPMS_ISR_ATTR ICACHE_RAM_ATTR
#else
#define TPMS_ISR_ATTR
#endif

float voltage[NUMSENSORS] = {};
int temperature[NUMSENSORS] = {};
float pressurePSI[NUMSENSORS] = {};
float pressureBAR[NUMSENSORS] = {};
bool updated[NUMSENSORS] = {};
unsigned long lastupdate[NUMSENSORS] = {};

namespace {

constexpr char kAccessPointSsid[] = "TPMS-Bridge";
constexpr char kPreferencesNamespace[] = "tpms433";
constexpr uint8_t kPacketLength = 9;
constexpr uint8_t kMaxDiscoveredSensors = 16;
constexpr unsigned long kUiSensorRetentionMs = 2UL * 60UL * 60UL * 1000UL;
constexpr unsigned long kSelectedSensorTimeoutMs = 70UL * 60UL * 1000UL;
constexpr float kPressurePsiPerKpa = 0.14503774f;
constexpr uint32_t kCc1101SpiClockHz = 200000;

constexpr int kCc1101CsPin = 15;
constexpr int kCc1101SckPin = 18;
constexpr int kCc1101MisoPin = 19;
constexpr int kCc1101MosiPin = 23;
constexpr int kCc1101Gdo0Pin = 5;
constexpr int kCc1101Gdo2Pin = 3;

struct DiscoveredSensor {
  bool used = false;
  uint32_t id = 0;
  uint8_t wheelCode = 0;
  uint8_t flags = 0;
  int pressureKPa = 0;
  int temperatureC = 0;
  int rssi = 0;
  int lqi = 0;
  unsigned long lastSeenMs = 0;
};

Preferences preferences;
WebServer webServer(80);
CC1101 radio = new Module(kCc1101CsPin, kCc1101Gdo0Pin, RADIOLIB_NC, kCc1101Gdo2Pin, SPI, SPISettings(kCc1101SpiClockHz, MSBFIRST, SPI_MODE0));
DiscoveredSensor discoveredSensors[kMaxDiscoveredSensors];
uint32_t selectedSensorIds[NUMSENSORS] = {};
uint32_t suggestedSensorIds[NUMSENSORS] = {};
bool wheelCodeZeroSeen = false;
bool wheelCodeFourSeen = false;
volatile bool receivedFlag = false;
volatile bool enableInterrupt = true;
bool radioReady = false;
int lastRadioError = RADIOLIB_ERR_NONE;
unsigned long lastRadioInitAttemptMs = 0;
IPAddress accessPointIp(192, 168, 4, 1);
IPAddress accessPointGateway(192, 168, 4, 1);
IPAddress accessPointSubnet(255, 255, 255, 0);

void setFlag();
void saveSelection(size_t slot);
void refreshSelection(size_t slot);
void refreshAllSelections();

const char* slotName(size_t slot) {
  static const char* const kSlotNames[NUMSENSORS] = { "FL", "FR", "RL", "RR" };
  return slot < NUMSENSORS ? kSlotNames[slot] : "--";
}

String formatSensorId(uint32_t id) {
  if (id == 0) {
    return String();
  }

  char buffer[9];
  snprintf(buffer, sizeof(buffer), "%08lX", static_cast<unsigned long>(id));
  return String(buffer);
}

void appendHtmlEscaped(String& html, const String& value) {
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    switch (character) {
      case '&':
        html += F("&amp;");
        break;
      case '<':
        html += F("&lt;");
        break;
      case '>':
        html += F("&gt;");
        break;
      case '"':
        html += F("&quot;");
        break;
      case '\'':
        html += F("&#39;");
        break;
      default:
        html += character;
        break;
    }
  }
}

void appendHtmlEscaped(String& html, const char* value) {
  appendHtmlEscaped(html, String(value));
}

void appendLinkButton(String& html, const String& href, const char* label, bool secondary = false) {
  html += F("<a class=\"");
  html += secondary ? F("btn secondary") : F("btn");
  html += F("\" href=\"");
  appendHtmlEscaped(html, href);
  html += F("\">");
  appendHtmlEscaped(html, label);
  html += F("</a>");
}

void logHttpRequest(const __FlashStringHelper* label) {
  Serial.print(F("[HTTP] "));
  Serial.print(label);
  Serial.print(F(" "));
  Serial.print(webServer.method() == HTTP_GET ? F("GET") : F("OTHER"));
  Serial.print(F(" "));
  Serial.println(webServer.uri());
}

bool applySelectionFromArgs(String* errorMessage = nullptr) {
  if (!webServer.hasArg("slot")) {
    if (errorMessage != nullptr) {
      *errorMessage = F("missing slot");
    }
    return false;
  }

  const long slotValue = webServer.arg("slot").toInt();
  if (slotValue < 0 || slotValue >= static_cast<long>(NUMSENSORS)) {
    if (errorMessage != nullptr) {
      *errorMessage = F("invalid slot");
    }
    return false;
  }

  const size_t slot = static_cast<size_t>(slotValue);
  String sensorId = webServer.arg("id");
  sensorId.trim();

  if (sensorId.length() == 0) {
    selectedSensorIds[slot] = 0;
    saveSelection(slot);
    refreshSelection(slot);
    return true;
  }

  char* endPtr = nullptr;
  const unsigned long parsedId = strtoul(sensorId.c_str(), &endPtr, 16);
  if (sensorId.c_str()[0] == '\0' || (endPtr != nullptr && *endPtr != '\0')) {
    if (errorMessage != nullptr) {
      *errorMessage = F("invalid id");
    }
    return false;
  }

  selectedSensorIds[slot] = static_cast<uint32_t>(parsedId);
  saveSelection(slot);
  refreshSelection(slot);
  return true;
}

void applyAutoAssignments() {
  bool changed = false;

  for (size_t slot = 0; slot < NUMSENSORS; ++slot) {
    if (suggestedSensorIds[slot] != 0 && selectedSensorIds[slot] != suggestedSensorIds[slot]) {
      selectedSensorIds[slot] = suggestedSensorIds[slot];
      saveSelection(slot);
      changed = true;
    }
  }

  if (changed) {
    refreshAllSelections();
  }
}

void redirectToRoot() {
  webServer.sendHeader("Location", "/", true);
  webServer.send(303, "text/plain", "");
}

const char* batteryStateLabel(uint8_t flags) {
  const uint8_t batteryBits = flags & 0x03;
  if (batteryBits == 0x03) {
    return "ok";
  }
  if (batteryBits == 0x00) {
    return "low";
  }
  return "unknown";
}

const char* mappingMode() {
  if (wheelCodeZeroSeen && !wheelCodeFourSeen) {
    return "0-based";
  }
  if (wheelCodeFourSeen && !wheelCodeZeroSeen) {
    return "1-based";
  }
  return "manual";
}

int slotFromWheelCode(uint8_t wheelCode) {
  if (wheelCodeZeroSeen && !wheelCodeFourSeen) {
    return wheelCode < NUMSENSORS ? static_cast<int>(wheelCode) : -1;
  }
  if (wheelCodeFourSeen && !wheelCodeZeroSeen) {
    return (wheelCode >= 1 && wheelCode <= NUMSENSORS) ? static_cast<int>(wheelCode) - 1 : -1;
  }
  return -1;
}

bool isVisibleInUi(const DiscoveredSensor& sensor, unsigned long nowMs) {
  return sensor.used && (nowMs - sensor.lastSeenMs) <= kUiSensorRetentionMs;
}

DiscoveredSensor* findDiscoveredSensor(uint32_t sensorId) {
  for (DiscoveredSensor& sensor : discoveredSensors) {
    if (sensor.used && sensor.id == sensorId) {
      return &sensor;
    }
  }
  return nullptr;
}

DiscoveredSensor* upsertDiscoveredSensor(uint32_t sensorId, unsigned long nowMs) {
  DiscoveredSensor* existing = findDiscoveredSensor(sensorId);
  if (existing != nullptr) {
    return existing;
  }

  DiscoveredSensor* oldest = &discoveredSensors[0];
  unsigned long oldestAge = 0;

  for (DiscoveredSensor& sensor : discoveredSensors) {
    if (!sensor.used) {
      sensor = DiscoveredSensor{};
      sensor.used = true;
      sensor.id = sensorId;
      return &sensor;
    }

    const unsigned long age = nowMs - sensor.lastSeenMs;
    if (age >= oldestAge) {
      oldestAge = age;
      oldest = &sensor;
    }
  }

  *oldest = DiscoveredSensor{};
  oldest->used = true;
  oldest->id = sensorId;
  return oldest;
}

void saveSelection(size_t slot) {
  char key[8];
  snprintf(key, sizeof(key), "slot%u", static_cast<unsigned>(slot));
  preferences.putULong(key, selectedSensorIds[slot]);
}

void loadSelections() {
  for (size_t slot = 0; slot < NUMSENSORS; ++slot) {
    char key[8];
    snprintf(key, sizeof(key), "slot%u", static_cast<unsigned>(slot));
    selectedSensorIds[slot] = preferences.getULong(key, 0);
  }
}

void resetSlotValues(size_t slot) {
  pressureBAR[slot] = 0.0f;
  pressurePSI[slot] = 0.0f;
  temperature[slot] = 0;
  voltage[slot] = 0.0f;
  updated[slot] = false;
  lastupdate[slot] = 0;
}

void publishSensorToSlot(size_t slot, const DiscoveredSensor& sensor) {
  pressureBAR[slot] = static_cast<float>(sensor.pressureKPa) / 100.0f;
  pressurePSI[slot] = static_cast<float>(sensor.pressureKPa) * kPressurePsiPerKpa;
  temperature[slot] = sensor.temperatureC;
  voltage[slot] = 0.0f;
  lastupdate[slot] = sensor.lastSeenMs;
  updated[slot] = true;
}

void refreshSelection(size_t slot) {
  if (selectedSensorIds[slot] == 0) {
    resetSlotValues(slot);
    return;
  }

  DiscoveredSensor* sensor = findDiscoveredSensor(selectedSensorIds[slot]);
  if (sensor == nullptr) {
    resetSlotValues(slot);
    return;
  }

  publishSensorToSlot(slot, *sensor);
}

void refreshAllSelections() {
  for (size_t slot = 0; slot < NUMSENSORS; ++slot) {
    refreshSelection(slot);
  }
}

bool rebuildSuggestions() {
  uint32_t newSuggestions[NUMSENSORS] = {};
  unsigned long newestSeenMs[NUMSENSORS] = {};
  const unsigned long nowMs = millis();

  for (const DiscoveredSensor& sensor : discoveredSensors) {
    if (!isVisibleInUi(sensor, nowMs)) {
      continue;
    }

    const int slot = slotFromWheelCode(sensor.wheelCode);
    if (slot < 0 || slot >= static_cast<int>(NUMSENSORS)) {
      continue;
    }

    if (sensor.lastSeenMs >= newestSeenMs[slot]) {
      newestSeenMs[slot] = sensor.lastSeenMs;
      newSuggestions[slot] = sensor.id;
    }
  }

  bool changed = false;
  for (size_t slot = 0; slot < NUMSENSORS; ++slot) {
    if (suggestedSensorIds[slot] != newSuggestions[slot]) {
      suggestedSensorIds[slot] = newSuggestions[slot];
      changed = true;
    }
  }
  return changed;
}

void noteWheelCode(uint8_t wheelCode) {
  if (wheelCode == 0) {
    wheelCodeZeroSeen = true;
  }
  if (wheelCode == NUMSENSORS) {
    wheelCodeFourSeen = true;
  }
}

bool ensureRadioState(const __FlashStringHelper* stepName, int state) {
  if (state == RADIOLIB_ERR_NONE) {
    lastRadioError = RADIOLIB_ERR_NONE;
    return true;
  }

  lastRadioError = state;
  radioReady = false;
  Serial.print(F("[CC1101] "));
  Serial.print(stepName);
  Serial.print(F(" failed, code "));
  Serial.println(state);
  return false;
}

void handleRoot() {
  logHttpRequest(F("root"));

  const unsigned long nowMs = millis();
  String html;
  html.reserve(12000);

  html += F("<!DOCTYPE html><html lang=\"de\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>TPMS Auswahl</title><style>");
  html += F("*{box-sizing:border-box;}body{margin:0;font-family:Verdana,sans-serif;background:#f5f1e8;color:#1f2933;}main{max-width:980px;margin:0 auto;padding:16px;}section{background:#fff;border:1px solid #d6d3cc;border-radius:16px;padding:16px;margin-bottom:16px;box-shadow:0 8px 24px rgba(0,0,0,.05);}h1,h2{margin:0 0 10px;}p{margin:0 0 10px;line-height:1.4;}small{color:#66737d;}.status{margin:12px 0;padding:12px;border-radius:12px;background:#eef3f1;}.warn{background:#fff3e6;color:#9a4d00;}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px;}.card{border:1px solid #d6d3cc;border-radius:12px;padding:12px;background:#fcfbf9;overflow:hidden;}.metric{display:inline-block;margin:0 10px 8px 0;color:#43515c;}.actions{display:flex;flex-wrap:wrap;gap:8px;margin-top:12px;}.slot-actions{display:flex;flex-wrap:wrap;gap:6px;margin-top:10px;}.btn{display:inline-block;max-width:100%;padding:10px 12px;border-radius:999px;background:#0f766e;color:#fff;text-decoration:none;font-size:.95rem;white-space:normal;overflow-wrap:anywhere;}.btn.secondary{background:#e7ecea;color:#1f2933;}.btn.warn{background:#b45309;color:#fff;}code{font-family:Consolas,monospace;background:#f3f4f6;padding:2px 6px;border-radius:6px;}@media (max-width:700px){.actions,.slot-actions{flex-direction:column;align-items:stretch}.btn{width:100%;text-align:center;}}</style></head><body><main>");

  html += F("<section><h1>433 MHz Sensoren verwalten</h1><p>Kleine, serverseitig gerenderte Uebersicht. Nur die gewaehlten Sensoren werden an die bestehende RaceChrono-BLE-Schnittstelle weitergereicht.</p><div class=\"");
  html += radioReady ? F("status\">") : F("status warn\">");
  html += F("AP <code>");
  appendHtmlEscaped(html, kAccessPointSsid);
  html += F("</code> · <code>");
  appendHtmlEscaped(html, WiFi.softAPIP().toString());
  html += F("</code> · ");
  if (radioReady) {
    html += F("CC1101 bereit");
  } else {
    html += F("CC1101 Fehler ");
    html += String(lastRadioError);
    html += F(", neuer Versuch laeuft");
  }
  html += F("</div><div class=\"actions\">");
  appendLinkButton(html, String("/"), "Aktualisieren", true);
  appendLinkButton(html, String("/auto-assign"), "Auto-Vorschlaege uebernehmen");
  appendLinkButton(html, String("/api/sensors"), "JSON Diagnose", true);
  html += F("</div></section>");

  html += F("<section><h2>Aktuelle Auswahl</h2><div class=\"grid\">");
  for (size_t slot = 0; slot < NUMSENSORS; ++slot) {
    html += F("<article class=\"card\"><strong>");
    appendHtmlEscaped(html, slotName(slot));
    html += F("</strong><p>Gewaehlt: <code>");
    const String selectedId = formatSensorId(selectedSensorIds[slot]);
    html += selectedId.length() > 0 ? selectedId : String("-");
    html += F("</code></p><p>Vorschlag: <code>");
    const String suggestedId = formatSensorId(suggestedSensorIds[slot]);
    html += suggestedId.length() > 0 ? suggestedId : String("-");
    html += F("</code></p><div class=\"slot-actions\">");
    appendLinkButton(html, String("/select?slot=") + slot + F("&id="), "Leeren", true);
    html += F("</div></article>");
  }
  html += F("</div></section>");

  html += F("<section><h2>Erkannte Sensoren</h2>");
  bool hasVisibleSensors = false;
  for (const DiscoveredSensor& sensor : discoveredSensors) {
    if (!isVisibleInUi(sensor, nowMs)) {
      continue;
    }

    hasVisibleSensors = true;
    html += F("<article class=\"card\"><strong><code>");
    const String sensorId = formatSensorId(sensor.id);
    appendHtmlEscaped(html, sensorId);
    html += F("</code></strong><p>");
    const int slotHint = slotFromWheelCode(sensor.wheelCode);
    if (slotHint >= 0) {
      html += F("Position ");
      appendHtmlEscaped(html, slotName(static_cast<size_t>(slotHint)));
    } else {
      html += F("Radcode ");
      html += String(sensor.wheelCode);
    }
    html += F("</p><div>");
    html += F("<span class=\"metric\">Druck: ");
    html += String(static_cast<float>(sensor.pressureKPa) / 100.0f, 2);
    html += F(" bar</span>");
    html += F("<span class=\"metric\">Temp: ");
    html += String(sensor.temperatureC);
    html += F(" C</span>");
    html += F("<span class=\"metric\">RSSI: ");
    html += String(sensor.rssi);
    html += F(" dBm</span>");
    html += F("<span class=\"metric\">Batterie: ");
    appendHtmlEscaped(html, batteryStateLabel(sensor.flags));
    html += F("</span>");
    html += F("</div><div class=\"slot-actions\">");
    for (size_t slot = 0; slot < NUMSENSORS; ++slot) {
      appendLinkButton(html, String("/select?slot=") + slot + F("&id=") + sensorId, slotName(slot), selectedSensorIds[slot] != sensor.id);
    }
    html += F("</div></article>");
  }

  if (!hasVisibleSensors) {
    html += radioReady
      ? F("<p>Noch kein 433-MHz-TPMS-Paket empfangen.</p>")
      : F("<p>CC1101 ist noch nicht bereit. Die Initialisierung wird automatisch wiederholt.</p>");
  }

  html += F("</section></main></body></html>");

  webServer.sendHeader("Cache-Control", "no-store, max-age=0");
  webServer.send(200, "text/html; charset=utf-8", html);
}

void handleNotFound() {
  logHttpRequest(F("not-found"));
  String html;
  html.reserve(512);
  html += F("<!DOCTYPE html><html lang=\"de\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>Nicht gefunden</title></head><body style=\"font-family:Verdana,sans-serif;padding:20px;\"><h1>Seite nicht gefunden</h1><p>Bitte rufe die Weboberflaeche direkt ueber <a href=\"/\">http://192.168.4.1/</a> auf.</p></body></html>");
  webServer.send(404, "text/html; charset=utf-8", html);
}

void handleSensors() {
  logHttpRequest(F("api-sensors"));
  const unsigned long nowMs = millis();
  String json;
  json.reserve(4096);

  json += F("{\"ssid\":\"");
  json += kAccessPointSsid;
  json += F("\",\"ip\":\"");
  json += WiFi.softAPIP().toString();
  json += F("\",\"mappingMode\":\"");
  json += mappingMode();
  json += F("\",\"radioReady\":");
  json += radioReady ? F("true") : F("false");
  json += F(",\"radioError\":");
  json += String(lastRadioError);
  json += F(",\"slots\":[");

  for (size_t slot = 0; slot < NUMSENSORS; ++slot) {
    if (slot > 0) {
      json += ',';
    }

    json += F("{\"name\":\"");
    json += slotName(slot);
    json += F("\",\"selectedId\":\"");
    json += formatSensorId(selectedSensorIds[slot]);
    json += F("\",\"suggestedId\":\"");
    json += formatSensorId(suggestedSensorIds[slot]);
    json += F("\"}");
  }

  json += F("],\"sensors\":[");

  bool firstSensor = true;
  for (const DiscoveredSensor& sensor : discoveredSensors) {
    if (!isVisibleInUi(sensor, nowMs)) {
      continue;
    }

    if (!firstSensor) {
      json += ',';
    }
    firstSensor = false;

    const int slotHint = slotFromWheelCode(sensor.wheelCode);

    json += F("{\"id\":\"");
    json += formatSensorId(sensor.id);
    json += F("\",\"wheelCode\":");
    json += String(sensor.wheelCode);
    json += F(",\"slotHint\":\"");
    if (slotHint >= 0) {
      json += slotName(static_cast<size_t>(slotHint));
    }
    json += F("\",\"pressureKPa\":");
    json += String(sensor.pressureKPa);
    json += F(",\"pressureBar\":");
    json += String(static_cast<float>(sensor.pressureKPa) / 100.0f, 2);
    json += F(",\"pressurePsi\":");
    json += String(static_cast<float>(sensor.pressureKPa) * kPressurePsiPerKpa, 1);
    json += F(",\"temperatureC\":");
    json += String(sensor.temperatureC);
    json += F(",\"flags\":");
    json += String(sensor.flags);
    json += F(",\"battery\":\"");
    json += batteryStateLabel(sensor.flags);
    json += F("\",\"rssi\":");
    json += String(sensor.rssi);
    json += F(",\"lqi\":");
    json += String(sensor.lqi);
    json += F(",\"ageMs\":");
    json += String(nowMs - sensor.lastSeenMs);
    json += F("}");
  }

  json += F("]}");

  webServer.sendHeader("Cache-Control", "no-store, max-age=0");
  webServer.send(200, "application/json", json);
}

void handleSelect() {
  logHttpRequest(F("api-select"));
  String errorMessage;
  if (!applySelectionFromArgs(&errorMessage)) {
    webServer.send(400, "text/plain", errorMessage);
    return;
  }

  webServer.send(204, "text/plain", "");
}

void handleAutoAssign() {
  logHttpRequest(F("api-auto-assign"));
  applyAutoAssignments();
  webServer.send(204, "text/plain", "");
}

void handleSelectPageAction() {
  logHttpRequest(F("page-select"));
  String errorMessage;
  if (!applySelectionFromArgs(&errorMessage)) {
    webServer.send(400, "text/plain", errorMessage);
    return;
  }

  redirectToRoot();
}

void handleAutoAssignPageAction() {
  logHttpRequest(F("page-auto-assign"));
  applyAutoAssignments();
  redirectToRoot();
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAPConfig(accessPointIp, accessPointGateway, accessPointSubnet);
  WiFi.softAP(kAccessPointSsid);
  Serial.print(F("TPMS AP: "));
  Serial.print(kAccessPointSsid);
  Serial.print(F(" @ "));
  Serial.println(WiFi.softAPIP());
  Serial.println(F("TPMS UI: http://192.168.4.1/"));
}

void startWebServer() {
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/index.html", HTTP_GET, handleRoot);
  webServer.on("/select", HTTP_GET, handleSelectPageAction);
  webServer.on("/auto-assign", HTTP_GET, handleAutoAssignPageAction);
  webServer.on("/api/sensors", HTTP_GET, handleSensors);
  webServer.on("/api/select", HTTP_POST, handleSelect);
  webServer.on("/api/auto-assign", HTTP_POST, handleAutoAssign);
  webServer.on("/ping", HTTP_GET, []() {
    logHttpRequest(F("ping"));
    String response = F("ok ");
    response += WiFi.softAPIP().toString();
    response += F(" radio=");
    response += radioReady ? F("ready") : F("retry");
    webServer.send(200, "text/plain", response);
  });
  webServer.onNotFound(handleNotFound);
  webServer.begin();
  Serial.println(F("[HTTP] Web server started"));
}

void configureRadio() {
  lastRadioInitAttemptMs = millis();
  radioReady = false;
  SPI.begin(kCc1101SckPin, kCc1101MisoPin, kCc1101MosiPin, kCc1101CsPin);

  Serial.print(F("[CC1101] Initializing ... "));
  int state = radio.begin();
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println(F("success"));
  } else {
    Serial.println();
    const int partNum = radio.SPIgetRegValue(0x30);
    const int version = radio.SPIgetRegValue(0x31);
    Serial.print(F("[CC1101] PARTNUM raw 0x"));
    Serial.println(partNum, HEX);
    Serial.print(F("[CC1101] VERSION raw 0x"));
    Serial.println(version, HEX);
    ensureRadioState(F("begin"), state);
    return;
  }

  Serial.print(F("[CC1101] Partnumber "));
  Serial.println(radio.SPIgetRegValue(0x30), HEX);
  Serial.print(F("[CC1101] Version "));
  Serial.println(radio.getChipVersion(), HEX);

  if (!ensureRadioState(F("setFrequency"), radio.setFrequency(433.92f))) {
    return;
  }
  if (!ensureRadioState(F("setBitRate"), radio.setBitRate(19.2f))) {
    return;
  }
  if (!ensureRadioState(F("setRxBandwidth"), radio.setRxBandwidth(135.0f))) {
    return;
  }
  if (!ensureRadioState(F("fixedPacketLengthMode"), radio.fixedPacketLengthMode(kPacketLength))) {
    return;
  }

  radio.SPIwriteRegister(0x04, 0x00);
  radio.SPIwriteRegister(0x05, 0x1A);
  if (!ensureRadioState(F("setEncoding"), radio.setEncoding(RADIOLIB_ENCODING_MANCHESTER))) {
    return;
  }
  radio.setGdo0Action(setFlag, RISING);

  Serial.print(F("[CC1101] Starting to listen ... "));
  state = radio.startReceive();
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println(F("success"));
    radioReady = true;
    lastRadioError = RADIOLIB_ERR_NONE;
  } else {
    Serial.println();
    ensureRadioState(F("startReceive"), state);
  }
}

void TPMS_ISR_ATTR setFlag() {
  if (!enableInterrupt) {
    return;
  }

  receivedFlag = true;
}

void processReceivedPacket(const uint8_t* packet, int packetLength, int rssi, int lqi) {
  if (packetLength != kPacketLength) {
    Serial.print(F("[CC1101] Ignoring packet length "));
    Serial.println(packetLength);
    return;
  }

  uint8_t checksum = 0;
  for (int index = 0; index < packetLength - 1; ++index) {
    checksum ^= packet[index];
  }

  if (checksum != packet[packetLength - 1]) {
    Serial.println(F("[CC1101] Ignoring packet with wrong checksum"));
    return;
  }

  // Packet format: ID[0..3], wheel[4], flags+pressure high nibble[5], pressure low[6], temp[7], checksum[8].
  const uint32_t sensorId =
    (static_cast<uint32_t>(packet[0]) << 24) |
    (static_cast<uint32_t>(packet[1]) << 16) |
    (static_cast<uint32_t>(packet[2]) << 8) |
    static_cast<uint32_t>(packet[3]);

  const unsigned long nowMs = millis();
  DiscoveredSensor* sensor = upsertDiscoveredSensor(sensorId, nowMs);
  sensor->wheelCode = packet[4];
  sensor->flags = packet[5] >> 4;
  sensor->pressureKPa = ((packet[5] & 0x0F) << 8) | packet[6];
  sensor->temperatureC = static_cast<int8_t>(packet[7]);
  sensor->rssi = rssi;
  sensor->lqi = lqi;
  sensor->lastSeenMs = nowMs;

  noteWheelCode(sensor->wheelCode);
  rebuildSuggestions();

  for (size_t slot = 0; slot < NUMSENSORS; ++slot) {
    if (selectedSensorIds[slot] == sensorId) {
      publishSensorToSlot(slot, *sensor);
    }
  }

  Serial.print(F("433 TPMS "));
  Serial.print(formatSensorId(sensorId));
  Serial.print(F(" wheel "));
  Serial.print(sensor->wheelCode);
  Serial.print(F("  "));
  Serial.print(static_cast<float>(sensor->pressureKPa) / 100.0f, 2);
  Serial.print(F(" bar  "));
  Serial.print(sensor->temperatureC);
  Serial.print(F(" C  RSSI "));
  Serial.println(sensor->rssi);
}

void pollRadio() {
  if (!receivedFlag) {
    return;
  }

  enableInterrupt = false;
  receivedFlag = false;

  int packetLength = radio.getPacketLength();
  if (packetLength <= 0) {
    packetLength = kPacketLength;
  }

  uint8_t packet[16] = {};
  if (packetLength > static_cast<int>(sizeof(packet))) {
    packetLength = sizeof(packet);
  }

  const int state = radio.readData(packet, packetLength);
  if (state == RADIOLIB_ERR_NONE) {
    processReceivedPacket(packet, packetLength, static_cast<int>(radio.getRSSI()), radio.getLQI());
  } else {
    Serial.print(F("[CC1101] readData failed, code "));
    Serial.println(state);
  }

  const int restartState = radio.startReceive();
  if (restartState != RADIOLIB_ERR_NONE) {
    Serial.print(F("[CC1101] restart receive failed, code "));
    Serial.println(restartState);
  }

  enableInterrupt = true;
}

void expireSelectedSensors() {
  const unsigned long nowMs = millis();
  for (size_t slot = 0; slot < NUMSENSORS; ++slot) {
    if (selectedSensorIds[slot] == 0 || lastupdate[slot] == 0) {
      continue;
    }

    if ((nowMs - lastupdate[slot]) >= kSelectedSensorTimeoutMs && temperature[slot] != 0) {
      temperature[slot] = 0;
      Serial.print(slotName(slot));
      Serial.println(F(" no signal for 70 minutes"));
    }
  }
}

void printUpdatedSlots() {
  for (size_t slot = 0; slot < NUMSENSORS; ++slot) {
    if (!updated[slot] || selectedSensorIds[slot] == 0) {
      continue;
    }

    Serial.print(slotName(slot));
    Serial.print(F("  "));
    Serial.print(formatSensorId(selectedSensorIds[slot]));
    Serial.print(F("  "));
    Serial.print(pressureBAR[slot], 2);
    Serial.print(F(" bar  "));
    Serial.print(temperature[slot]);
    Serial.println(F(" C"));
    updated[slot] = false;
  }
}

}  // namespace

void startTpms() {
  for (size_t slot = 0; slot < NUMSENSORS; ++slot) {
    resetSlotValues(slot);
    selectedSensorIds[slot] = 0;
    suggestedSensorIds[slot] = 0;
  }

  preferences.begin(kPreferencesNamespace, false);
  loadSelections();
  refreshAllSelections();
  startAccessPoint();
  startWebServer();
  configureRadio();
}

void checkTpms() {
  webServer.handleClient();

  if (radioReady) {
    pollRadio();
  } else if (millis() - lastRadioInitAttemptMs >= 5000UL) {
    configureRadio();
  }

  expireSelectedSensors();
  printUpdatedSlots();
}