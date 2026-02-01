/*
  Garagenlueftung – Firmware v3.0.5 (aus Version 2.2.b mit Shelly Output)
  AP über Pin 1 definierbar
  © 2026 EuS Soft, 9428 Walzenhausen
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <math.h>

// ===== Display (SSD1306 128x64 I2C) =====
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

String pageWifi();
String pageSettings();
String pageManual();

// =======================================================
// Serial Logging (ASCII only)
// =======================================================
#define LOGI(fmt, ...) do { Serial.printf("[INFO] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOGW(fmt, ...) do { Serial.printf("[WARN] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOGE(fmt, ...) do { Serial.printf("[ERR ] " fmt "\n", ##__VA_ARGS__); } while(0)

// =======================================================
// Firmware Info
// =======================================================
static const char* FW_NAME      = "Garagenlueftung";
static const char* FW_VERSION = "v3.0.5";
static const char* FW_DATE    = "2026-02-01";
static const char* FW_COPYRIGHT = "© 2026 EuS Soft, 9428 Walzenhausen";



// =======================================================
// Hardware Pins (M5Stamp C3)  -> ggf. anpassen!
// =======================================================
// Relais-Logik: true = active LOW (sehr haeufig bei Relaisboards)
static const bool TOR_ACTIVE_LOW = true;
static const bool FAN_ACTIVE_LOW = true;

inline void writeOut(int pin, bool on, bool activeLow){
  digitalWrite(pin, (on ^ activeLow) ? HIGH : LOW);
}

bool wifiUsable() {
  wifi_mode_t m = WiFi.getMode();
  return (m == WIFI_MODE_STA || m == WIFI_MODE_AP || m == WIFI_MODE_APSTA);
}

bool shellySwitchSet(const char* ip, int ch, bool on) {

  if (!wifiUsable()) {
    LOGW("Shelly skipped: WiFi not usable");
    return false;
  }

  wifi_mode_t mode = WiFi.getMode();

  WiFiClient client;
  client.setTimeout(2000);

  LOGI("Shelly cmd=%s ip=%s ch=%d wifi_mode=%d",
       on ? "ON" : "OFF", ip, ch, mode);

  if (!client.connect(ip, 80)) {
    LOGE("Shelly not reachable: %s", ip);
    return false;
  }

  client.printf(
    "GET /rpc/Switch.Set?id=%d&on=%s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Connection: close\r\n\r\n",
    ch,
    on ? "true" : "false",
    ip
  );

  while (client.connected() || client.available()) {
    if (client.available()) client.read();
  }

  client.stop();
  LOGI("Shelly %s -> %s", ip, on ? "ON" : "OFF");
  return true;
}


// =======================================================
// Shelly Status lesen (ON / OFF)
// =======================================================
bool shellySwitchGet(const char* ip, int ch, bool& isOn) {
  if (!wifiUsable()) {
    LOGW("Shelly skipped (WiFi not usable)");
    return false;
  }

  WiFiClient client;
  
  client.setTimeout(2000);

  if (!client.connect(ip, 80)) {
    LOGE("Shelly not reachable (GET): %s", ip);
    return false;
  }

  client.printf(
    "GET /rpc/Switch.GetStatus?id=%d HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Connection: close\r\n\r\n",
    ch,
    ip
  );

  String payload;
  bool body = false;

  while (client.connected() || client.available()) {
    if (!client.available()) continue;
    String line = client.readStringUntil('\n');
    if (line == "\r") {
      body = true;
      continue;
    }
    if (body) payload += line;
  }

  client.stop();

  // sehr einfache JSON-Auswertung (reicht hier)
  if (payload.indexOf("\"output\":true") >= 0) {
    isOn = true;
    return true;
  }
  if (payload.indexOf("\"output\":false") >= 0) {
    isOn = false;
    return true;
  }

  LOGW("Shelly status parse failed: %s", payload.c_str());
  return false;
}
// Shelly Lüfter Konfiguration
// static const char* SHELLY_IP = "192.168.4.10";
// static const int   SHELLY_CH = 0;



static const int PIN_TOR_RELAIS = 4;
static const int PIN_LUEFTER    = 5;
static const int PIN_START_BTN  = 6;
static const int PIN_ABORT_BTN  = 7;   // gegen GND

static const int PIN_MODE_BTN   = 10;  // Preset-Taste (gegen GND)

// M5Stamp C3: 21/20 sind herausgefuehrt (siehe Foto)
static const int I2C_SDA        = 21;
static const int I2C_SCL        = 20;

// ===== AP / Setup Policy (Innen + Aussen gleich) =====
static const int PIN_SETUP_BTN = 1;     // gegen GND, INPUT_PULLUP
static const uint32_t AP_TIMEOUT_MS = 5UL * 60UL * 1000UL; // 5 Minuten

bool apRunning = false;
bool apPermanent = false;
uint32_t apStartedMs = 0;

// =======================================================
// Defaults (nur beim ersten Start)
// =======================================================
static const int   DEF_VENT_MIN  = 120;
static const float DEF_CLOSE_S   = 17.5;
static const int   DEF_PAUSE_S   = 45;

static const int IMPULS_MS = 500;

// Preset Defaults (Minuten)
static const int DEF_PRESET_1 = 30;
static const int DEF_PRESET_2 = 60;
static const int DEF_PRESET_3 = 120;
static const int DEF_PRESET_4 = 180;



// =======================================================
// Display config
// =======================================================
#define OLED_W 128
#define OLED_H 32
#define OLED_RESET -1
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, OLED_RESET);

bool displayOk = false;
uint8_t displayAddr = 0x3C;
unsigned long lastUiMs = 0;

// =======================================================
// Storage
// =======================================================
Preferences prefs;
static const char* PREF_NS = "garage";

struct Settings {
  int vent_min;
  float close_sec;
  int pause_sec;

  String device_name;

  String wifi_ssid;
  String wifi_pass;

  String ap_ssid;
  String ap_pass;

  String mqtt_server;
  String mqtt_user;
  String mqtt_pass;
  int mqtt_port;

  bool   ip_static;
  String ip_addr;
  String ip_gw;
  String ip_mask; 
  String ip_dns;

  int preset_min[4];
  int preset_sel; // 0..3
  int wifi_mode;   // 0=AP_ONLY, 1=STA_ONLY, 2=AUTO

  String shelly_ip;
  int    shelly_ch;

} cfg;

// =======================================================
// Lüfter-Abstraktion (GPIO oder Shelly)
// =======================================================
void luefter(bool on) {

  if (useShelly()) {

    if (!wifiUsable()) {
      LOGW("Shelly requested but WiFi not usable");
      return;
    }

    shellySwitchSet(cfg.shelly_ip.c_str(), cfg.shelly_ch, on);
    LOGI("Luefter Shelly -> %s (ip=%s ch=%d)",
         on ? "ON" : "OFF",
         cfg.shelly_ip.c_str(),
         cfg.shelly_ch);

  } else {

    writeOut(PIN_LUEFTER, on, FAN_ACTIVE_LOW);
    LOGI("Luefter GPIO -> %s", on ? "ON" : "OFF");
  }
}



// =======================================================
// Runtime State
// =======================================================
enum class CycleState {
  IDLE,
  MOVING_TO_VENT,
  VENTING,
  PAUSE,
  STOPPING_BEFORE_CLOSE,   // 👈 NEU
  CLOSING,
  ABORTED,
  FINISHED
};


bool cycleRunning = false;
CycleState state = CycleState::IDLE;

unsigned long stateUntilMs = 0;
unsigned long ventEndMs = 0;
unsigned long lastBtnMs = 0;
unsigned long lastModeBtnMs = 0;

WebServer server(80);

// =======================================================
// MQTT
// =======================================================
WiFiClient espClient;
PubSubClient mqtt(espClient);

String topicBase;
String topicCmd;
String topicStatus;
String topicRemaining;
String topicAvail;

String topicCfgVentMin;
String topicCfgPauseSec;
String topicCfgCloseSec;
String topicSetVentMin;
String topicSetPauseSec;
String topicSetCloseSec;

// preset via MQTT
String topicCfgPresetSel;
String topicSetPresetSel;

unsigned long lastMqttRetryMs = 0;
unsigned long lastMqttPublishMs = 0;
String lastPubState;
String lastPubRemaining;

// =======================================================
// Helpers
// =======================================================
String macSuffix() {
  uint64_t chipid = ESP.getEfuseMac();
  char buf[7];
  snprintf(buf, sizeof(buf), "%06llX", (chipid >> 24) & 0xFFFFFF);
  return String(buf);
}
String sanitizeHost(const String& in) {
  String s = in;
  s.trim();
  s.toLowerCase();
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-');
    if (ok) out += c;
    else if (c == ' ' || c == '_') out += '-';
  }
  while (out.indexOf("--") >= 0) out.replace("--", "-");
  if (out.startsWith("-")) out.remove(0, 1);
  if (out.endsWith("-")) out.remove(out.length() - 1);
  return out;
}

bool useShelly() {
  return cfg.shelly_ip.length() > 0;
}

// v2.2.0: stabile MQTT Device-ID
String mqttDeviceId() {
  String n = sanitizeHost(cfg.device_name);
  if (n.length() > 0) return n;
  return macSuffix();   // Fallback
}
// v2.2.0: zentrale Topic-Erzeugung
void generateMqttTopics() {
  String id = mqttDeviceId();

  topicBase      = "eus/" + id;
  topicCmd       = topicBase + "/cmd";
  topicStatus    = topicBase + "/status";
  topicRemaining = topicBase + "/remaining";
  topicAvail     = topicBase + "/availability";

  topicCfgVentMin  = topicBase + "/cfg/vent_min";
  topicCfgPauseSec = topicBase + "/cfg/pause_sec";
  topicCfgCloseSec = topicBase + "/cfg/close_sec";

  topicSetVentMin  = topicBase + "/set/vent_min";
  topicSetPauseSec = topicBase + "/set/pause_sec";
  topicSetCloseSec = topicBase + "/set/close_sec";

  topicCfgPresetSel = topicBase + "/cfg/preset_sel";
  topicSetPresetSel = topicBase + "/set/preset_sel";

  LOGI("MQTT device id: %s", id.c_str());
}


String hostname() {
  String base = sanitizeHost(cfg.device_name);
  if (base.length() == 0) base = "garage";
  if (base.length() > 24) base = base.substring(0, 24);
  return base + "-" + macSuffix();
}

String urlEncode(const String& s) {
  const char *hex = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    bool safe =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) out += (char)c;
    else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

String supportMailtoHref() {
  String subject = String(FW_NAME) + " " + FW_VERSION + " Support";
  String body;
  body += "Hallo Emil,\r\n\r\n";
  body += "Mein Geraet:\r\n";
  body += "- Name: " + (cfg.device_name.length() ? cfg.device_name : String("(kein Name)")) + "\r\n";
  body += "- Hostname: " + hostname() + "\r\n";
  body += "- IP: " + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("(offline)")) + "\r\n";
  body += "- Firmware: " + String(FW_VERSION) + " (" + String(FW_DATE) + ")\r\n";
  if (topicBase.length()) body += "- MQTT TopicBase: " + topicBase + "\r\n";
  body += "\r\nProblem:\r\n";
  return "mailto:emil.huber@gmx.ch?subject=" + urlEncode(subject) + "&body=" + urlEncode(body);
}

String stateStr() {
  switch (state) {
    case CycleState::IDLE:                 return "IDLE";
    case CycleState::MOVING_TO_VENT:       return "TOR -> LUEFTUNG";
    case CycleState::VENTING:              return "LUEFTET";
    case CycleState::PAUSE:                return "TOR OFFEN";
    case CycleState::STOPPING_BEFORE_CLOSE:return "STOP -> SCHLIESSEN"; // 👈 NEU
    case CycleState::CLOSING:              return "TOR SCHLIESST";
    case CycleState::ABORTED:              return "ABGEBROCHEN";
    case CycleState::FINISHED:             return "FERTIG";
  }
  return "?";
}


String remainingTimeStr() {
  if (stateUntilMs == 0) return "–";

  long ms = (long)stateUntilMs - (long)millis();
  if (ms < 0) ms = 0;

  int sekunden = (ms / 1000) % 60;
  int minuten  = (ms / 1000) / 60;

  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d min", minuten, sekunden);
  return String(buf);
}

String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}

String apiStatusJson() {
  String ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("");
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;

  String j = "{";
  j += "\"state\":\"" + jsonEscape(stateStr()) + "\",";
  j += "\"remaining\":\"" + jsonEscape(remainingTimeStr()) + "\",";
  j += "\"running\":" + String(cycleRunning ? "true" : "false") + ",";
  j += "\"ip\":\"" + jsonEscape(ip) + "\",";
  j += "\"rssi\":" + String(rssi) + ",";
  j += "\"name\":\"" + jsonEscape(cfg.device_name) + "\",";
  j += "\"host\":\"" + jsonEscape(hostname()) + "\",";
  j += "\"preset_sel\":" + String(cfg.preset_sel) + ",";
  j += "\"preset_min\":" + String(cfg.preset_min[cfg.preset_sel]) + ",";
  j += "\"vent_min\":" + String(cfg.vent_min);
  j += "}";
  return j;
}

enum WifiMode : int {
  WIFI_AP_ONLY = 0,
  WIFI_STA_ONLY = 1,
  WIFI_AUTO    = 2
};

static const int DEF_WIFI_MODE = WIFI_AUTO;
  

void pulseTor() {
  writeOut(PIN_TOR_RELAIS, true, TOR_ACTIVE_LOW);

  unsigned long t0 = millis();
  while (millis() - t0 < IMPULS_MS) {
    server.handleClient();   // 🔑 AP bleibt erreichbar
    delay(1);
  }

  writeOut(PIN_TOR_RELAIS, false, TOR_ACTIVE_LOW);
}


// =======================================================
// Settings
// =======================================================
void loadSettings() {
  // prefs.putInt("wifi_mode", cfg.wifi_mode);
  // prefs.begin(PREF_NS, false);
  prefs.begin(PREF_NS, true);
  cfg.wifi_mode = prefs.getInt("wifi_mode", DEF_WIFI_MODE);

  cfg.vent_min    = prefs.getInt("vent",  DEF_VENT_MIN);
  cfg.close_sec   = prefs.getFloat("close", DEF_CLOSE_S);
  cfg.pause_sec   = prefs.getInt("pause", DEF_PAUSE_S);

  cfg.device_name = prefs.getString("devname", "");

  cfg.wifi_ssid   = prefs.getString("ssid", "");
  cfg.wifi_pass   = prefs.getString("pass", "");

  cfg.ap_ssid     = prefs.getString("ap_ssid", "");
  cfg.ap_pass     = prefs.getString("ap_pass", "");

  cfg.mqtt_server = prefs.getString("mqtt_srv", "");
  cfg.mqtt_user   = prefs.getString("mqtt_user", "");
  cfg.mqtt_pass   = prefs.getString("mqtt_pass", "");
  cfg.mqtt_port   = prefs.getInt("mqtt_port", 1883);

  cfg.ip_static = prefs.getBool("ip_static", false);
  cfg.ip_addr   = prefs.getString("ip_addr", "");
  cfg.ip_gw     = prefs.getString("ip_gw", "");
  cfg.ip_mask   = prefs.getString("ip_mask", "");
  cfg.ip_dns    = prefs.getString("ip_dns", "");

  cfg.preset_min[0] = prefs.getInt("p1", DEF_PRESET_1);
  cfg.preset_min[1] = prefs.getInt("p2", DEF_PRESET_2);
  cfg.preset_min[2] = prefs.getInt("p3", DEF_PRESET_3);
  cfg.preset_min[3] = prefs.getInt("p4", DEF_PRESET_4);
  cfg.preset_sel    = prefs.getInt("psel", 2);
  if (cfg.preset_sel < 0) cfg.preset_sel = 0;
  if (cfg.preset_sel > 3) cfg.preset_sel = 3;

  if (cfg.vent_min <= 0) cfg.vent_min = cfg.preset_min[cfg.preset_sel];

  cfg.shelly_ip = prefs.getString("shelly_ip", "192.168.4.10");
  cfg.shelly_ch = prefs.getInt("shelly_ch", 0);

  prefs.end();

  LOGI("Settings loaded");
  LOGI("Name: %s", cfg.device_name.c_str());
  LOGI("SSID: %s", cfg.wifi_ssid.c_str());
  LOGI("Preset sel=P%d, minutes=%d", cfg.preset_sel + 1, cfg.preset_min[cfg.preset_sel]);
}

void saveSettings() {
  prefs.begin(PREF_NS, false);
  prefs.putInt("wifi_mode", cfg.wifi_mode);
  prefs.putInt("vent", cfg.vent_min);
  prefs.putFloat("close", cfg.close_sec);
  prefs.putInt("pause", cfg.pause_sec);

  prefs.putString("devname", cfg.device_name);

  prefs.putString("ap_ssid", cfg.ap_ssid);
  prefs.putString("ap_pass", cfg.ap_pass);

  prefs.putString("mqtt_srv",  cfg.mqtt_server);
  prefs.putString("mqtt_user", cfg.mqtt_user);
  prefs.putString("mqtt_pass", cfg.mqtt_pass);
  prefs.putInt("mqtt_port",    cfg.mqtt_port);

  prefs.putBool("ip_static", cfg.ip_static);
  prefs.putString("ip_addr", cfg.ip_addr);
  prefs.putString("ip_gw",   cfg.ip_gw);
  prefs.putString("ip_mask", cfg.ip_mask);
  prefs.putString("ip_dns",  cfg.ip_dns);

  prefs.putInt("p1", cfg.preset_min[0]);
  prefs.putInt("p2", cfg.preset_min[1]);
  prefs.putInt("p3", cfg.preset_min[2]);
  prefs.putInt("p4", cfg.preset_min[3]);
  prefs.putInt("psel", cfg.preset_sel);

  prefs.putString("shelly_ip", cfg.shelly_ip);
  prefs.putInt("shelly_ch", cfg.shelly_ch);

  prefs.end();
}

void saveWiFi(const String& ssid, const String& pass) {
  prefs.begin(PREF_NS, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();

  cfg.wifi_ssid = ssid;
  cfg.wifi_pass = pass;
}

// =======================================================
// WiFi
// =======================================================
bool connectWiFi() {
  if (cfg.wifi_ssid.isEmpty()) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname().c_str());

  if (!cfg.ip_static) {
    WiFi.config(IPAddress(INADDR_NONE), IPAddress(INADDR_NONE), IPAddress(INADDR_NONE), IPAddress(INADDR_NONE));
    LOGI("DHCP active");
  } else {
    IPAddress ip, gw, mask, dns;
    bool okIp   = ip.fromString(cfg.ip_addr);
    bool okGw   = gw.fromString(cfg.ip_gw);
    bool okMask = mask.fromString(cfg.ip_mask);
    bool okDns  = (cfg.ip_dns.length() == 0) ? true : dns.fromString(cfg.ip_dns);

    if (okIp && okGw && okMask && okDns) {
      bool applied = false;
      if (cfg.ip_dns.length() > 0) applied = WiFi.config(ip, gw, mask, dns);
      else                         applied = WiFi.config(ip, gw, mask);

      LOGI("Static IP: %s (apply=%s)", cfg.ip_addr.c_str(), applied ? "ok" : "FAIL");
    } else {
      LOGW("Static-IP values invalid -> DHCP");
      WiFi.config(IPAddress(INADDR_NONE), IPAddress(INADDR_NONE), IPAddress(INADDR_NONE), IPAddress(INADDR_NONE));
    }
  }

  LOGI("WLAN connect: %s", cfg.wifi_ssid.c_str());
  WiFi.begin(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str());

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    LOGI("WLAN connected, IP: %s", WiFi.localIP().toString().c_str());
  }
  return WiFi.status() == WL_CONNECTED;
}

String defaultApSsid() {
  String base = sanitizeHost(cfg.device_name);
  if (base.length() == 0) base = "garage";
  if (base.length() > 12) base = base.substring(0, 12);
  return "Setup-" + base + "-" + macSuffix();
}
String defaultApPass() {
  return "garage-" + macSuffix();
}

bool hasWifiCreds() {
  String s = cfg.wifi_ssid;
  s.trim();
  return s.length() > 0;
}
bool staAllowed() {
  return cfg.wifi_mode == WIFI_STA_ONLY || cfg.wifi_mode == WIFI_AUTO;
}

bool staHasCreds() {
  String s = cfg.wifi_ssid; s.trim();
  return s.length() > 0;
}

void startSetupAPManaged(bool permanent) {
  startSetupAP(); // nutzt SSID/PW wie bisher (Setup-... / garage-<mac>)
  apRunning = true;
  apPermanent = permanent;
  apStartedMs = millis();

  if (apPermanent)
    LOGW("AP mode: PERMANENT");
  else
    LOGW("AP mode: TEMPORARY (5 min)");

}


void startSetupAP() {
  WiFi.mode(WIFI_AP);

  String apSsid = cfg.ap_ssid; apSsid.trim();
  if (apSsid.length() == 0) apSsid = defaultApSsid();

  String apPass = cfg.ap_pass;
  if (apPass.length() == 0) apPass = defaultApPass();
  if (apPass.length() < 8)  apPass = defaultApPass();

  WiFi.softAP(apSsid.c_str(), apPass.c_str());

  LOGW("Setup-AP started");
  LOGW("  SSID: %s", apSsid.c_str());
  LOGW("  PW  : %s", apPass.c_str());
  LOGW("  IP  : %s", WiFi.softAPIP().toString().c_str());
}




// =======================================================
// Cycle Logic
// =======================================================
void abortCycle() {
  cycleRunning = false;
  luefter(false);
  pulseTor();

  state = CycleState::IDLE;   // 👈 WICHTIG
  stateUntilMs = 0;
}


void startCycle() {
  if (cycleRunning) return;

  cycleRunning = true;
  pulseTor();

  state = CycleState::MOVING_TO_VENT;
  stateUntilMs = millis() + (unsigned long)(cfg.close_sec * 1000.0f);
}

void handlePauseEnd() {
  const unsigned long safetyMs = 1000UL;
  unsigned long pauseMs = (unsigned long)cfg.pause_sec * 1000UL;
  unsigned long torMs   = (unsigned long)(cfg.close_sec * 1000.0f);

  if (torMs <= safetyMs) {
    // Torfahrzeit extrem klein -> immer direkt schliessen (sicher)
    LOGW("Torfahrzeit <= 1s -> CLOSE");
    pulseTor();
    state = CycleState::CLOSING;
    stateUntilMs = millis() + torMs + 2000UL;
    return;
  }

  if (pauseMs < (torMs - safetyMs)) {
    LOGI("Pause < Torfahrzeit -> STOP, dann CLOSE");
    pulseTor();
    state = CycleState::STOPPING_BEFORE_CLOSE;
    stateUntilMs = millis() + 1000UL;
  } else {
    LOGI("Pause >= Torfahrzeit -> CLOSE");
    pulseTor();
    state = CycleState::CLOSING;
    stateUntilMs = millis() + torMs + 2000UL;
  }
}



void loopCycle() {
  unsigned long now = millis();

  // Zyklus läuft nicht -> Rückkehr zu IDLE prüfen
  if (!cycleRunning) {
    if (state == CycleState::FINISHED && now >= stateUntilMs) {
      state = CycleState::IDLE;
      stateUntilMs = 0;
    }
    return;
  }

  // Übergang zu VENTING
  if (state == CycleState::MOVING_TO_VENT && now >= stateUntilMs) {
    pulseTor();
    luefter(true);

    state = CycleState::VENTING;
    ventEndMs = now + (unsigned long)cfg.vent_min * 60000UL;
    stateUntilMs = ventEndMs;
    return;
  }

  // Übergang zu PAUSE
  if (state == CycleState::VENTING && now >= stateUntilMs) {
    pulseTor();
    luefter(false);

    state = CycleState::PAUSE;
    stateUntilMs = now + (unsigned long)cfg.pause_sec * 1000UL;
    return;
  }

  // Übergang zu CLOSING (mit intelligenter Impulslogik)
  if (state == CycleState::PAUSE && now >= stateUntilMs) {
    handlePauseEnd();
    return;
  }

  // Übergang STOPPING -> CLOSING (delay-frei)
  if (state == CycleState::STOPPING_BEFORE_CLOSE && now >= stateUntilMs) {

    pulseTor();   // Jetzt schliessen

    state = CycleState::CLOSING;
    stateUntilMs = millis() + (unsigned long)(cfg.close_sec * 1000.0f) + 2000UL;
    return;
  }


  // Übergang zu FINISHED
  if (state == CycleState::CLOSING && now >= stateUntilMs) {
    cycleRunning = false;
    state = CycleState::FINISHED;
    stateUntilMs = millis() + 3000UL;  // 3 Sekunden "FERTIG" anzeigen
    return;
  }
}

// =======================================================
// Preset Button (MODE) + Apply
// =======================================================
void applyPresetSelection(int sel) {
  if (sel < 0) sel = 0;
  if (sel > 3) sel = 3;
  cfg.preset_sel = sel;
  cfg.vent_min = cfg.preset_min[cfg.preset_sel];
  saveSettings();
  LOGI("Preset -> P%d = %d min", cfg.preset_sel + 1, cfg.vent_min);
}

void handleModeButton() {
  if (cycleRunning) return;

  if (digitalRead(PIN_MODE_BTN) == LOW && millis() - lastModeBtnMs > 350) {
    lastModeBtnMs = millis();
    int next = (cfg.preset_sel + 1) % 4;
    applyPresetSelection(next);
  }
}

// =======================================================
// MQTT Logic
// =======================================================
bool mqttEnabled() {
  return (WiFi.status() == WL_CONNECTED) && (cfg.mqtt_server.length() > 0);
}

bool mqttTcpReachable(const String& host, uint16_t port) {
  WiFiClient test;
  test.setTimeout(1500);
  bool ok = test.connect(host.c_str(), port);
  test.stop();
  return ok;
}

void mqttPublishCfg(bool force=false){
  if (!mqttEnabled() || !mqtt.connected()) return;

  char buf[16];
  snprintf(buf, sizeof(buf), "%d", cfg.vent_min);
  mqtt.publish(topicCfgVentMin.c_str(), buf, true);

  snprintf(buf, sizeof(buf), "%d", cfg.pause_sec);
  mqtt.publish(topicCfgPauseSec.c_str(), buf, true);

  snprintf(buf, sizeof(buf), "%.1f", cfg.close_sec);
  mqtt.publish(topicCfgCloseSec.c_str(), buf, true);

  char pselBuf[4];
  snprintf(pselBuf, sizeof(pselBuf), "%d", cfg.preset_sel + 1);
  mqtt.publish(topicCfgPresetSel.c_str(), pselBuf, true);

  if(force) LOGI("MQTT cfg published (retained)");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  String t(topic);
  LOGI("MQTT: %s -> %s", t.c_str(), msg.c_str());

  if (t == topicCmd) {
    if (msg == "start") startCycle();
    else if (msg == "stop") abortCycle();
    return;
  }

  if (t == topicSetPresetSel) {
    if (cycleRunning) {
      LOGW("Preset change ignored (cycle running).");
      mqttPublishCfg(true);
      return;
    }
    int sel = msg.toInt(); // 1..4
    sel -= 1;              // 0..3
    if (sel < 0) sel = 0;
    if (sel > 3) sel = 3;

    applyPresetSelection(sel);
    mqttPublishCfg(true);
    return;
  }

  if (t == topicSetVentMin || t == topicSetPauseSec || t == topicSetCloseSec) {
    if (cycleRunning) {
      LOGW("Config change ignored (cycle running).");
      mqttPublishCfg(true);
      return;
    }

    bool changed = false;

    if (t == topicSetVentMin) {
      int v = msg.toInt();
      if (v < 1) v = 1;
      if (v > 600) v = 600;
      if (cfg.vent_min != v) { cfg.vent_min = v; changed = true; }
    }

    if (t == topicSetPauseSec) {
      int p = msg.toInt();
      if (p < 0) p = 0;
      if (p > 600) p = 600;
      if (cfg.pause_sec != p) { cfg.pause_sec = p; changed = true; }
    }

    if (t == topicSetCloseSec) {
      float c = msg.toFloat();
      if (c < 1.0f) c = 1.0f;
      if (c > 60.0f) c = 60.0f;
      if (fabs(cfg.close_sec - c) > 0.05f) { cfg.close_sec = c; changed = true; }
    }

    if (changed) {
      saveSettings();
      LOGI("Config saved.");
      mqttPublishCfg(true);
    } else {
      mqttPublishCfg(false);
    }
    return;
  }
}

void mqttPublish(bool force = false) {
  if (!mqttEnabled() || !mqtt.connected()) return;

  String s = stateStr();
  String r = remainingTimeStr();

  unsigned long now = millis();
  bool periodic = (now - lastMqttPublishMs) >= 10000UL;

  if (force || periodic || s != lastPubState || r != lastPubRemaining) {
    mqtt.publish(topicStatus.c_str(), s.c_str(), true);
    mqtt.publish(topicRemaining.c_str(), r.c_str(), true);
    mqtt.publish(topicAvail.c_str(), "online", true);

    lastPubState = s;
    lastPubRemaining = r;
    lastMqttPublishMs = now;
  }
}

void mqttPublishHomeAssistantDiscovery() {
  if (!mqtt.connected()) return;

  String id   = mqttDeviceId();      // z.B. "garage-ost"
  String base = "homeassistant";

  // === gemeinsamer Device-Block (WICHTIG für HA!) ===
  String deviceJson = "\"device\":{";
  deviceJson += "\"identifiers\":[\"" + id + "\"],";
  deviceJson += "\"name\":\"" + (cfg.device_name.length() ? cfg.device_name : "Garagenlüftung") + "\",";
  deviceJson += "\"manufacturer\":\"EuS Soft\",";
  deviceJson += "\"model\":\"Garagenlüftung\",";
  deviceJson += "\"sw_version\":\"" + String(FW_VERSION) + "\"";
  deviceJson += "},";

String topicStartBtn = base + "/button/" + id + "/start/config";
String payloadStartBtn = "{";
payloadStartBtn += deviceJson;
payloadStartBtn += "\"name\":\"Garagenlüftung Start\",";
payloadStartBtn += "\"command_topic\":\"" + topicCmd + "\",";
payloadStartBtn += "\"payload_press\":\"start\",";
payloadStartBtn += "\"availability_topic\":\"" + topicAvail + "\",";
payloadStartBtn += "\"payload_available\":\"online\",";
payloadStartBtn += "\"payload_not_available\":\"offline\",";
payloadStartBtn += "\"unique_id\":\"" + id + "_start_btn\"";
payloadStartBtn += "}";

mqtt.publish(topicStartBtn.c_str(), payloadStartBtn.c_str(), true);

  // ==================================================
// === Switch: Abbrechen (separat)
// ==================================================
String topicAbortBtn = base + "/button/" + id + "/abort/config";
String payloadAbortBtn = "{";
payloadAbortBtn += deviceJson;
payloadAbortBtn += "\"name\":\"Garagenlüftung Abbrechen\",";
payloadAbortBtn += "\"command_topic\":\"" + topicCmd + "\",";
payloadAbortBtn += "\"payload_press\":\"stop\",";
payloadAbortBtn += "\"availability_topic\":\"" + topicAvail + "\",";
payloadAbortBtn += "\"payload_available\":\"online\",";
payloadAbortBtn += "\"payload_not_available\":\"offline\",";
payloadAbortBtn += "\"unique_id\":\"" + id + "_abort_btn\"";
payloadAbortBtn += "}";

mqtt.publish(topicAbortBtn.c_str(), payloadAbortBtn.c_str(), true);

  // ==================================================
  // === Sensor: Status
  // ==================================================
  String topicStatusCfg = base + "/sensor/" + id + "/status/config";
  String payloadStatus = "{";
  payloadStatus += deviceJson;
  payloadStatus += "\"name\":\"Garagenlüftung Status\",";
  payloadStatus += "\"state_topic\":\"" + topicStatus + "\",";
  payloadStatus += "\"availability_topic\":\"" + topicAvail + "\",";
  payloadStatus += "\"payload_available\":\"online\",";
  payloadStatus += "\"payload_not_available\":\"offline\",";
  payloadStatus += "\"unique_id\":\"" + id + "_status\"";
  payloadStatus += "}";

  mqtt.publish(topicStatusCfg.c_str(), payloadStatus.c_str(), true);

  // ==================================================
  // === Sensor: Remaining Time
  // ==================================================
  String topicRemCfg = base + "/sensor/" + id + "/remaining/config";
  String payloadRem = "{";
  payloadRem += deviceJson;
  payloadRem += "\"name\":\"Garagenlüftung Restzeit\",";
  payloadRem += "\"state_topic\":\"" + topicRemaining + "\",";
  payloadRem += "\"availability_topic\":\"" + topicAvail + "\",";
  payloadRem += "\"payload_available\":\"online\",";
  payloadRem += "\"payload_not_available\":\"offline\",";
  payloadRem += "\"unique_id\":\"" + id + "_remaining\"";
  payloadRem += "}";

  mqtt.publish(topicRemCfg.c_str(), payloadRem.c_str(), true);

  // ==================================================
  // === Sensor: Aktives Preset
  // ==================================================
  String topicPresetCfg = base + "/sensor/" + id + "/preset/config";
  String payloadPreset = "{";
  payloadPreset += deviceJson;
  payloadPreset += "\"name\":\"Garagenlüftung Preset\",";
  payloadPreset += "\"state_topic\":\"" + topicCfgPresetSel + "\",";
  payloadPreset += "\"value_template\":\"{{ value | int }}\",";
  payloadPreset += "\"availability_topic\":\"" + topicAvail + "\",";
  payloadPreset += "\"payload_available\":\"online\",";
  payloadPreset += "\"payload_not_available\":\"offline\",";
  payloadPreset += "\"unique_id\":\"" + id + "_preset\"";
  payloadPreset += "}";

  mqtt.publish(topicPresetCfg.c_str(), payloadPreset.c_str(), true);
  // ==================================================
  // === Sensor: Firmware-Version
  // ==================================================
  String topicFw = base + "/sensor/" + id + "/firmware/config";
  String payloadFw = "{";
  payloadFw += deviceJson;
  payloadFw += "\"name\":\"Firmware-Version\",";
  payloadFw += "\"state_topic\":\"eus/" + id + "/info/firmware\",";
  payloadFw += "\"unique_id\":\"" + id + "_firmware\"";
  payloadFw += "}";

  mqtt.publish(topicFw.c_str(), payloadFw.c_str(), true);

  // ==================================================
  // === Select: Preset-Auswahl (1–4)
  // ==================================================
  String topicPresetSelect = base + "/select/" + id + "/preset_select/config";
  String payloadPresetSelect = "{";
  payloadPresetSelect += deviceJson;
  payloadPresetSelect += "\"name\":\"Lüftungsprofil\",";
  payloadPresetSelect += "\"state_topic\":\"eus/" + id + "/cfg/preset_sel\",";
  payloadPresetSelect += "\"command_topic\":\"eus/" + id + "/set/preset_sel\",";
  payloadPresetSelect += "\"options\":[\"1\",\"2\",\"3\",\"4\"],";
  payloadPresetSelect += "\"unique_id\":\"" + id + "_preset_select\"";
  payloadPresetSelect += "}";

  mqtt.publish(topicPresetSelect.c_str(), payloadPresetSelect.c_str(), true);

  LOGI("Home Assistant Discovery topics published");
}


void mqttEnsureConnected() {
  if (!mqttEnabled()) return;
  if (mqtt.connected()) return;

  unsigned long now = millis();
  if (now - lastMqttRetryMs < 10000UL) return;
  lastMqttRetryMs = now;

  uint16_t port = (cfg.mqtt_port > 0 && cfg.mqtt_port < 65536) ? (uint16_t)cfg.mqtt_port : 1883;

  if (!mqttTcpReachable(cfg.mqtt_server, port)) {
    LOGE("MQTT TCP not reachable: %s:%u", cfg.mqtt_server.c_str(), port);
    return;
  }

  mqtt.setServer(cfg.mqtt_server.c_str(), port);
  mqtt.setBufferSize(1024);
  mqtt.setCallback(mqttCallback);

  String clientId = "GarageLueftung-" + mqttDeviceId();
  LOGI("MQTT connect to %s:%u ...", cfg.mqtt_server.c_str(), port);

  bool ok = false;
  if (cfg.mqtt_user.length() > 0) {
    ok = mqtt.connect(
      clientId.c_str(),
      cfg.mqtt_user.c_str(),
      cfg.mqtt_pass.c_str(),
      topicAvail.c_str(), 0, true, "offline"
    );
  } else {
    ok = mqtt.connect(clientId.c_str(), topicAvail.c_str(), 0, true, "offline");
  }

  if (ok) {
    LOGI("MQTT connected");
        // Firmware-Info publizieren (retained)
    String fwTopic = "eus/" + mqttDeviceId() + "/info/firmware";
    String fwValue = String(FW_VERSION) + " (" + FW_DATE + ")";
    mqtt.publish(fwTopic.c_str(), fwValue.c_str(), true);
    mqtt.subscribe(topicCmd.c_str());
    mqtt.subscribe(topicSetVentMin.c_str());
    mqtt.subscribe(topicSetPauseSec.c_str());
    mqtt.subscribe(topicSetCloseSec.c_str());
    mqtt.subscribe(topicSetPresetSel.c_str());

    mqttPublishCfg(true);
    mqttPublishHomeAssistantDiscovery(); 
    mqttPublish(true);
  } else {
    LOGE("MQTT connect failed, state=%d", mqtt.state());
  }
}

// =======================================================
// Display UI (SAFE init - works even without display)
// =======================================================
bool tryProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return (Wire.endTransmission() == 0);
}

void uiInit() {
  LOGI("UI init (safe) SDA=%d SCL=%d", I2C_SDA, I2C_SCL);

  // prevent floating if display is not connected
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(20);

  bool p3c = tryProbe(0x3C);
  bool p3d = tryProbe(0x3D);

  if (!p3c && !p3d) {
    displayOk = false;
    LOGW("No OLED found (0x3C/0x3D). UI disabled.");
    return;
  }

  uint8_t addr = p3c ? 0x3C : 0x3D;

  if (!display.begin(SSD1306_SWITCHCAPVCC, addr)) {
    displayOk = false;
    LOGW("OLED begin failed at 0x%02X. UI disabled.", addr);
    return;
  }

  displayAddr = addr;
  displayOk = true;
  LOGI("OLED OK at 0x%02X", displayAddr);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Garagenlueftung");
  display.println(FW_VERSION);
  display.display();
}

String twoDigits(int v) {
  if (v < 10) return "0" + String(v);
  return String(v);
}



void uiTick() {
  if (!displayOk) return;
  constexpr uint8_t LINE_H = 8;
  unsigned long now = millis();
  if (now - lastUiMs < 300) return;
  lastUiMs = now;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Zeile 1: Titel
  display.setCursor(0, LINE_H * 0);
  String title = cfg.device_name.length() ? cfg.device_name : "Garage";
  if (title.length() > 21) title = title.substring(0, 21);
  display.print(title);

  // Zeile 2: Preset
  display.setCursor(0, LINE_H * 1);
  display.print("P");
  display.print(cfg.preset_sel + 1);
  display.print(": ");
  display.print(cfg.preset_min[cfg.preset_sel]);
  display.print(" min");

  // Zeile 3: Status / Zeit
  display.setCursor(0, LINE_H * 2);
  if (!cycleRunning) {
    display.print("Bereit");
  } else {
    long ms = (long)stateUntilMs - (long)millis();
    if (ms < 0) ms = 0;
    int sec = (ms / 1000) % 60;
    int min = (ms / 1000) / 60;
    display.print("Laeuft ");
    display.print(min);
    display.print(":");
    display.print(twoDigits(sec));
  }

  // ✅ Zeile 4: Netzwerk (DAS HAT GEFEHLT)
  display.setCursor(0, LINE_H * 3);

  if (WiFi.status() == WL_CONNECTED) {
    display.print(WiFi.localIP());
  }
  else if (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA) {
    int n = WiFi.softAPgetStationNum();
    display.print("AP: ");
    display.print(n);
    display.print(" Client");
  }
  else {
    display.print("offline");
  }

  // ✅ GANZ WICHTIG
  display.display();
}

// =======================================================
// Web UI
// =======================================================
const char* COMMON_CSS = R"CSS(
:root{--bg1:#0ea5e9;--bg2:#22c55e;--card:rgba(255,255,255,.92);--txt:#0f172a;--muted:#475569}
*{box-sizing:border-box}
body{
  font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;
  margin:0; min-height:100vh; color:var(--txt);
  background:
    radial-gradient(1200px 600px at 15% 10%, rgba(255,255,255,.35), transparent 60%),
    linear-gradient(135deg,var(--bg1),var(--bg2));
}
.wrap{max-width:520px;margin:0 auto;padding:18px}
.card{
  background:var(--card); border-radius:18px; padding:18px;
  box-shadow:0 12px 30px rgba(0,0,0,.18);
  backdrop-filter:blur(8px)
}
h2{margin:0 0 10px 0;font-weight:750;letter-spacing:.2px}
p{margin:10px 0;color:var(--muted)}
b{color:var(--txt)}
a{color:#0f172a;text-decoration:none;font-weight:600}
a:hover{text-decoration:underline}
button,input,select{
  width:100%; padding:12px; margin:8px 0; border-radius:12px;
  border:0; font-size:16px
}
input,select{border:1px solid rgba(15,23,42,.15);background:#fff}
button{
  cursor:pointer;font-weight:750;color:#fff;
  background:linear-gradient(135deg,#2563eb,#06b6d4);
  box-shadow:0 10px 18px rgba(37,99,235,.25)
}
button:active{transform:translateY(1px)}
form[action='/stop'] button{
  background:linear-gradient(135deg,#ef4444,#f97316);
  box-shadow:0 10px 18px rgba(239,68,68,.22)
}
hr{border:0;border-top:1px solid rgba(15,23,42,.12);margin:14px 0}
small{color:rgba(15,23,42,.7)}

/* ---ab  UI v3.0.3: Fokus auf Start, Preset nur optional --- */
.kpi{
  background:rgba(15,23,42,.06);
  border:1px solid rgba(15,23,42,.10);
  border-radius:16px;
  padding:14px 14px 12px 14px;
  margin:14px 0;
  text-align:center;
}
.kpiLabel{font-size:13px;color:var(--muted);letter-spacing:.2px}
.kpiValue{font-size:34px;font-weight:850;line-height:1.1;margin-top:4px}
.kpiSub{font-size:13px;color:rgba(15,23,42,.72);margin-top:6px}

.rowline{display:flex;gap:10px;align-items:center;margin-top:8px}
.rowline label{font-size:13px;color:var(--muted);white-space:nowrap}
.rowline select{flex:1;margin:0}
.rowline button{width:auto;margin:0;padding:12px 14px;border-radius:12px}

button.big{padding:14px;font-size:18px}
button.secondary{
  background:linear-gradient(135deg,#334155,#0f172a);
  box-shadow:0 10px 18px rgba(15,23,42,.18)
}
button.secondary:disabled{opacity:.55;cursor:not-allowed}
select:disabled{opacity:.70}
@media (prefers-color-scheme: dark){
  :root{--card:rgba(15,23,42,.78);--txt:#e5e7eb;--muted:#cbd5e1}
  body{color:var(--txt)}
  a{color:#e5e7eb}
  input,select{background:rgba(2,6,23,.65);color:#e5e7eb;border:1px solid rgba(255,255,255,.15)}
  small{color:rgba(229,231,235,.75)}
  .kpi{background:rgba(255,255,255,.08);border:1px solid rgba(255,255,255,.12)}
  .kpiSub{color:rgba(229,231,235,.78)}
  hr{border-top:1px solid rgba(255,255,255,.14)}
}
)CSS";

String pageBegin(const String& title) {
  String h;
  h += "<!doctype html><html><head>";
  h += "<meta charset='utf-8'><meta name='viewport' content='width=device-width'>";
  h += "<style>"; h += COMMON_CSS; h += "</style>";
  h += "</head><body><div class='wrap'><div class='card'>";
  h += "<h2>" + title + "</h2>";
  return h;
}

String pageEnd() {
  String h;
  h += "<hr><small>";
  h += String(FW_COPYRIGHT) + " · ";
  h += "<a href='" + supportMailtoHref() + "'>emil.huber@gmx.ch</a><br>";
  h += String(FW_NAME) + " " + FW_VERSION + " (" + FW_DATE + ")";
  h += "</small></div></div></body></html>";
  return h;
}

String pageStatus() {
  String title = cfg.device_name.length() ? cfg.device_name : String("Garage Lüftung");
  String h = pageBegin(title);

  // Status / Restzeit / Netzwerk
  h += "<p>Status: <b id='st'>" + stateStr() + "</b></p>";
  h += "<p>Restzeit: <span id='rem'>" + remainingTimeStr() + "</span></p>";
  h += "<p><small id='net'></small></p>";

  // Lüftungszeit prominent
  h += "<div class='kpi'>";
  h +=   "<div class='kpiLabel'>Vorgewählte Lüftungszeit</div>";
  h +=   "<div class='kpiValue' id='ptime'>" + String(cfg.preset_min[cfg.preset_sel]) + " min</div>";
  h +=   "<div class='kpiSub' id='pname'>Preset P" + String(cfg.preset_sel + 1) + "</div>";
  h += "</div>";

  // 👉 Action Button (leer, wird per JS gesetzt)
  h += "<div id='actionBtn'></div>";

  // Preset-Auswahl
  h += "<hr>";
  h += "<form onsubmit='return false;' class='rowline'>";
  h += "<select id='psel' name='psel' "
       "onfocus='window.uiLock=true' "
       "onblur='window.uiLock=false' ";

  if (!(state == CycleState::IDLE || state == CycleState::FINISHED)) {
    h += "disabled ";
  }

  h += ">";

  for (int i = 0; i < 4; i++) {
    h += "<option value='" + String(i + 1) + "' ";
    if (cfg.preset_sel == i) h += "selected";
    h += ">P" + String(i + 1) + " (" + String(cfg.preset_min[i]) + " min)</option>";
  }
  h += "</select>";

  if (state == CycleState::IDLE || state == CycleState::FINISHED) {
    h += "<button type='button' class='secondary' onclick='savePreset()'>💾</button>";
  } else {
    h += "<button type='button' class='secondary' disabled>💾</button>";
  }

  h += "</form>";
  h += "<small>Preset-Änderung nur wenn kein Vorgang läuft.</small>";

  // Menü
  h += "<hr>";
  h += "<a href='/settings'>⚙ Einstellungen</a><br>";
  h += "<a href='/wifi'>📶 WLAN</a><br>";
  h += "<a href='/update'>⬆️ OTA Update</a><br>";
  h += "<a href='/manual'>📘 Handbuch</a><br>";
  h += "<a href='/debug'>🧪 Debug</a><br>";
  h += "<a href='/restart'>♻️ Neustart</a><br>";

  // =======================
  // Live Update Script
  // =======================
  h += "<script>";
  h += "window.uiLock=false;";
  h += "let lastState=null;";
  h += "let lastRunning=null;";

  h += "async function tick(){";
  h += " if(window.uiLock) return;";
  h += " try{";
  h += "  const r=await fetch('/api/status',{cache:'no-store'});";
  h += "  if(!r.ok) return;";
  h += "  const d=await r.json();";

  // 🔑 State-Übergang sauber erkennen
  h += "  const prevState=lastState;";
  h += "  lastState=d.state;";
  h += "  if(prevState!==null && d.state==='IDLE' && prevState!=='IDLE'){";
  h += "    location.reload(); return;";
  h += "  }";

  // DOM refs
  h += "  const st=document.getElementById('st');";
  h += "  const rem=document.getElementById('rem');";
  h += "  const net=document.getElementById('net');";
  h += "  const ptime=document.getElementById('ptime');";
  h += "  const pname=document.getElementById('pname');";
  h += "  const psel=document.getElementById('psel');";
  h += "  const action=document.getElementById('actionBtn');";

  // Start / Abbrechen Button
  h += "  if(action && d.running!==lastRunning){";
  h += "    if(d.running){";
  h += "      action.innerHTML=\"<form method='POST' action='/stop'>"
       "<button class='big'>⛔ Abbrechen</button></form>\";";
  h += "    }else{";
  h += "      action.innerHTML=\"<form method='POST' action='/start'>"
       "<button class='big'>▶ Start</button></form>\";";
  h += "    }";
  h += "    lastRunning=d.running;";
  h += "  }";

  // Textfelder
  h += "  if(st) st.textContent=d.state||'';";
  h += "  if(rem) rem.textContent=d.remaining||'';";
  h += "  if(ptime && d.preset_min!=null) ptime.textContent=d.preset_min+' min';";
  h += "  if(pname && typeof d.preset_sel==='number') pname.textContent='Preset P'+(d.preset_sel+1);";
  h += "  if(psel && typeof d.preset_sel==='number') psel.value=String(d.preset_sel+1);";

  // Netzwerk
  h += "  if(net){";
  h += "    let s='';";
  h += "    if(d.ip) s+='IP: '+d.ip;";
  h += "    if(typeof d.rssi==='number' && d.ip) s+=' · ';";
  h += "    if(typeof d.rssi==='number') s+='WLAN: '+d.rssi+' dBm';";
  h += "    if(d.host) s+=' · '+d.host+'.local';";
  h += "    net.textContent=s;";
  h += "  }";

  h += " }catch(e){}";
  h += "}";

  // Preset speichern
  h += "async function savePreset(){";
  h += " const psel=document.getElementById('psel');";
  h += " if(!psel) return;";
  h += " try{";
  h += "  const r=await fetch('/preset',{method:'POST',"
       "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
       "body:'psel='+psel.value});";
  h += "  if(!r.ok) return;";
  h += "  const j=await r.json();";
  h += "  const ptime=document.getElementById('ptime');";
  h += "  const pname=document.getElementById('pname');";
  h += "  if(ptime) ptime.textContent=j.minutes+' min';";
  h += "  if(pname) pname.textContent='Preset P'+(j.preset+1);";
  h += " }catch(e){}";
  h += "}";

  h += "tick(); setInterval(tick,1000);";
  h += "</script>";

  h += pageEnd();
  return h;
}


String pageUpdate() {
  String h = pageBegin("Firmware Update (OTA)");
  h += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  h += "<input type='file' name='update' accept='.bin' required>";
  h += "<button type='submit'>⬆️ Update starten</button>";
  h += "</form><hr><a href='/'>⬅ Zurück</a>";
  h += pageEnd();
  return h;
}

String pageSettings() {
  String h = pageBegin("Einstellungen");
  h += "<form method='POST' action='/save'>";

  h += "<hr><b>Gerät</b><br>";
  h += "Gerätename (optional)<br>";
  h += "<input name='devname' value='" + cfg.device_name + "' placeholder='z.B. Garage-Ost'><br>";
  h += "<small>Hostname: " + hostname() + ".local (wirksam nach Neustart)</small><br>";

  h += "<hr><b>Presets (Offenzeit)</b><br>";
  h += "Preset 1 (min)<br><input name='p1' value='" + String(cfg.preset_min[0]) + "'>";
  h += "Preset 2 (min)<br><input name='p2' value='" + String(cfg.preset_min[1]) + "'>";
  h += "Preset 3 (min)<br><input name='p3' value='" + String(cfg.preset_min[2]) + "'>";
  h += "Preset 4 (min)<br><input name='p4' value='" + String(cfg.preset_min[3]) + "'>";

  h += "<br>Aktives Preset (Vorwahl)<br>";
  h += "<select name='psel'>";
  for (int i = 0; i < 4; i++) {
    h += "<option value='" + String(i + 1) + "' ";
    if (cfg.preset_sel == i) h += "selected";
    h += ">P" + String(i + 1) + " (" + String(cfg.preset_min[i]) + " min)</option>";
  }
  h += "</select>";
  h += "<small>MODE-Taste wechselt Preset; im laufenden Cycle wird nicht umgeschaltet.</small><br>";

  h += "<hr><b>Zeit</b><br>";
  h += "Torfahrzeit bis Lüftungsschlitz (s)";
  h += "<input name='close' value='" + String(cfg.close_sec, 1) + "'>";
  h += "Pause (s)";
  h += "<input name='pause' value='" + String(cfg.pause_sec) + "'>";

  h += "<hr><b>MQTT</b><br>";
  h += "MQTT Server (IP/Hostname, leer = aus)";
  h += "<input name='mqtt_srv' value='" + cfg.mqtt_server + "'>";
  h += "MQTT Port";
  h += "<input name='mqtt_port' value='" + String(cfg.mqtt_port) + "'>";
  h += "MQTT User (optional)";
  h += "<input name='mqtt_user' value='" + cfg.mqtt_user + "'>";
  h += "MQTT Passwort (optional, leer lassen = unverändert)";
  h += "<input name='mqtt_pass' type='password' value=''>";

  h += "<hr><b>Shelly (Lüfter)</b><br>";
  h += "Shelly IP<br>";
  h += "<input name='shelly_ip' value='" + cfg.shelly_ip + "' placeholder='192.168.4.10'><br>";

  h += "Shelly Kanal<br>";
  h += "<input name='shelly_ch' value='" + String(cfg.shelly_ch) + "' placeholder='0'><br>";


  h += "<button type='submit'>💾 Speichern</button>";
  h += "</form><hr><a href='/'>⬅ Zurück</a>";
  h += pageEnd();
  return h;
}

String pageWifi() {
  String h = pageBegin("WLAN-Einstellungen");

  h += "<form method='POST' action='/wifi'>";
  h += "<b>WLAN (Station-Modus)</b><br>";
  h += "SSID<br><input name='ssid' value='" + cfg.wifi_ssid + "' placeholder='z.B. FRITZ!Box'><br>";
  h += "Passwort<br><input name='pass' type='password' value='" + cfg.wifi_pass + "' placeholder='●●●●●●●●'><br>";

  h += "<hr><b>IP-Konfiguration</b><br>";
  h += "<label><input type='checkbox' name='ip_static' " + String(cfg.ip_static ? "checked" : "") + "> Statische IP verwenden</label><br>";
  h += "IP-Adresse<br><input name='ip_addr' value='" + cfg.ip_addr + "' placeholder='192.168.1.50'><br>";
  h += "Gateway<br><input name='ip_gw' value='" + cfg.ip_gw + "' placeholder='192.168.1.1'><br>";
  h += "Subnetzmaske<br><input name='ip_mask' value='" + cfg.ip_mask + "' placeholder='255.255.255.0'><br>";
  h += "DNS-Server<br><input name='ip_dns' value='" + cfg.ip_dns + "' placeholder='8.8.8.8'><br>";

  h += "<hr><b>Access-Point (Setup-Modus)</b><br>";
  h += "SSID (optional)<br><input name='ap_ssid' value='" + cfg.ap_ssid + "' placeholder='z.B. Garage-Setup'><br>";
  h += "Passwort (min. 8 Zeichen)<br><input name='ap_pass' type='password' value='" + cfg.ap_pass + "'><br>";
  h += "<small>Wird verwendet, wenn kein WLAN verfügbar ist oder Setup-Taste (GPIO1) beim Start gedrückt wird.</small><br>";

  h += "<hr><button type='submit'>💾 Speichern</button>";
  h += "</form>";

  h += "<hr><a href='/'>⬅ Zurück</a>";
  h += pageEnd();
  return h;
}

  String pageDebug() {
    String h = pageBegin("Debug");

    h += "<pre style='font-size:13px;line-height:1.4'>";

    h += "FW: " + String(FW_VERSION) + " (" + FW_DATE + ")\n";
    h += "Uptime: " + String(millis() / 1000) + " s\n\n";

    h += "State: " + stateStr() + "\n";
    h += "Cycle running: " + String(cycleRunning ? "yes" : "no") + "\n";
    h += "Remaining: " + remainingTimeStr() + "\n\n";

    h += "Preset sel: P" + String(cfg.preset_sel + 1) + "\n";
    h += "Preset minutes: " + String(cfg.preset_min[cfg.preset_sel]) + "\n";
    h += "Vent_min: " + String(cfg.vent_min) + "\n\n";

    h += "WiFi mode: " + String(cfg.wifi_mode) + "\n";
    h += "WiFi status: ";
    h += (WiFi.status() == WL_CONNECTED ? "CONNECTED\n" : "DISCONNECTED\n");

    if (WiFi.status() == WL_CONNECTED) {
      h += "IP: " + WiFi.localIP().toString() + "\n";
      h += "RSSI: " + String(WiFi.RSSI()) + " dBm\n";
      h += "Hostname: " + hostname() + ".local\n";
    }

    h += "\nMQTT:\n";
    h += " Enabled: " + String(mqttEnabled() ? "yes" : "no") + "\n";
    h += " Connected: " + String(mqtt.connected() ? "yes" : "no") + "\n";
    h += " TopicBase: " + topicBase + "\n";

    h += "</pre>";

    h += "<hr><a href='/'>⬅ Zurück</a>";
    h += pageEnd();
    return h;
  }



String pageManual() {
  String h;
  h += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'>");
  h += F("<title>Garagenlüftung Bedienungsanleitung v3.0.5</title>");
  h += F("<style>"
         "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;margin:0;padding:0;background:#eef3f7;color:#0f172a;}"
         "header{background:#0ea5e9;color:#fff;padding:16px;text-align:center;}"
         "h1{margin:0;font-size:1.6em;}"
         "main{max-width:900px;margin:0 auto;padding:14px;}"
         "section{background:#fff;margin:10px 0;padding:16px;border-radius:10px;box-shadow:0 2px 6px rgba(0,0,0,.1);}"
         "h2{color:#0ea5e9;margin-top:0;}"
         "code{background:#f3f4f6;padding:2px 4px;border-radius:4px;}"
         ".badge{display:inline-block;background:#0ea5e9;color:#fff;border-radius:6px;padding:3px 8px;margin-left:6px;font-size:0.8em;}"
         ".note{padding:8px 10px;border-radius:8px;margin:10px 0;}"
         ".ok{background:#dcfce7;border-left:4px solid #16a34a;}"
         ".warn{background:#fef3c7;border-left:4px solid #f59e0b;}"
         "footer{text-align:center;color:#475569;padding:14px;font-size:0.9em;}"
         "</style></head><body>");

  h += F("<header><h1>Garagenlüftung <span class='badge'>v3.0.5</span> <span class='badge'>2026-01-10</span></h1>"
         "<p>© 2026 EuS Soft, 9428 Walzenhausen</p></header><main>");

  h += F("<section><h2>Überblick</h2>"
         "<p>Die <b>Garagenlüftung</b> steuert ein Garagentor und einen Lüfter zeitgesteuert über einen ESP32-Controller."
         " Sie kann sowohl über Hardwaretasten, das <b>Web-UI</b> als auch per <b>MQTT / Home Assistant</b> gesteuert werden.</p>"
         "<div class='note ok'><b>Neu in Version 3.0.5:</b><ul><li>Start ist die primäre Aktion (direkt nach der angezeigten Lüftungszeit)</li><li>Lüftungszeit wird prominent dargestellt</li><li>Preset-Änderung ist bewusst sekundär (eine Zeile, kleiner Button)</li><li>Preset-Änderung bleibt während Zyklus gesperrt</li></ul></div></section>");

  h += F("<section><h2>Web-Oberfläche</h2>"
         "<ul>"
         "<li><b>/</b> – Status, Preset-Auswahl, Start/Abbrechen</li>"
         "<li><b>/wifi</b> – WLAN-Konfiguration</li>"
         "<li><b>/settings</b> – Geräte- und MQTT-Einstellungen</li>"
         "<li><b>/update</b> – OTA-Firmware-Update</li>"
         "<li><b>/debug</b> – Status-Infos</li>"
         "<li><b>/manual</b> – diese Anleitung</li>"
         "<li><b>/restart</b> – Neustart des Geräts</li>"
         "</ul>"
         "<p>Die Startseite zeigt Status, Restzeit und die <b>vorgewählte Lüftungszeit</b>. In der Regel genügt es, <b>Start</b> zu drücken. Das Preset kann optional darunter geändert werden (nur wenn kein Cycle läuft).</p>"
         "</section>");

  h += F("<section><h2>MQTT</h2>"
         "<p>Basis: <code>eus/&lt;device&gt;/...</code></p><ul>"
         "<li><code>cmd</code> – start | stop</li>"
         "<li><code>status</code> – aktueller Status</li>"
         "<li><code>remaining</code> – Restzeit (s)</li>"
         "<li><code>cfg/preset_sel</code> – aktives Preset</li>"
         "<li><code>set/preset_sel</code> – Preset setzen (1–4)</li>"
         "</ul>"
         "<p>Beispiel: <code>mosquitto_pub -t 'eus/garage-ost/cmd' -m 'start'</code></p>"
         "</section>");

  h += F("<section><h2>Home Assistant</h2>"
         "<p>Unterstützt MQTT-Discovery: automatisch erkannt.</p>"
         "<ul>"
         "<li>Schaltflächen: Start / Abbrechen</li>"
         "<li>Sensoren: Status, Restzeit, Firmware, Preset</li>"
         "<li>Select: Lüftungsprofil (P1–P4)</li>"
         "</ul></section>");

  h += F("<section><h2>Debug / Support</h2>"
         "<ul><li><b>/debug:</b> Gerätestatus &amp; MQTT-Daten</li>"
         "<li><b>/manual:</b> Bedienungsanleitung</li>"
         "<li>Support: <a href='mailto:emil.huber@gmx.ch'>emil.huber@gmx.ch</a></li></ul></section>");

  h += F("</main><footer>Garagenlüftung – Firmware v3.0.5 · © 2025 EuS Soft</footer></body></html>");
  return h;
}




// =======================================================
// Web Routes
// =======================================================
void setupWeb() {
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", pageStatus()); });
  server.on("/settings", HTTP_GET, []() { server.send(200, "text/html", pageSettings()); });
  server.on("/wifi", HTTP_GET, []() { server.send(200, "text/html", pageWifi()); });
  server.on("/restart", HTTP_GET, []() {
    server.send(200, "text/plain", "Neustart in 0.5 Sekunden...");
    delay(500);
    ESP.restart();
  });

  server.on("/api/status", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", apiStatusJson());
  });

  server.on("/debug", HTTP_GET, []() {
    server.send(200, "text/html", pageDebug());
  });


  // === OTA Update Seite (GET) ===
  server.on("/update", HTTP_GET, []() {
    server.send(200, "text/html", pageUpdate());
  });

  server.on("/update", HTTP_POST,
    []() {
      bool ok = !Update.hasError();
      server.send(200, "text/plain", ok ? "Update OK. Neustart..." : "Update FEHLER!");
      server.client().stop();
      delay(500);
      ESP.restart();
    },
    []() {
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        LOGI("OTA Start: %s", upload.filename.c_str());
        if (mqtt.connected()) mqtt.disconnect();
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) LOGI("OTA End: %u bytes", upload.totalSize);
        else Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.end();
        LOGW("OTA aborted");
      }
      delay(0);
    }
  );
  
  server.on("/manual", HTTP_GET, []() { server.send(200, "text/html", pageManual()); });

  server.on("/start", HTTP_POST, []() {
    startCycle();
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/stop", HTTP_POST, []() {
    abortCycle();
    server.sendHeader("Location", "/");
    server.send(303);
  });

    server.on("/preset", HTTP_POST, []() {
    if (!server.hasArg("psel")) {
      server.send(400, "application/json", "{}");
      return;
    }

    int sel = server.arg("psel").toInt(); // 1..4
    sel -= 1;                             // 0..3
    if (sel < 0) sel = 0;
    if (sel > 3) sel = 3;

    if (!cycleRunning) {
      applyPresetSelection(sel);
      lastMqttRetryMs = 0;
      mqttPublishCfg(true);
    }

    // JSON Antwort für Live-UI
    String json =
      "{\"preset\":" + String(cfg.preset_sel) +
      ",\"minutes\":" + String(cfg.preset_min[cfg.preset_sel]) + "}";

    server.send(200, "application/json; charset=utf-8", json);
  });


  // Settings speichern
  server.on("/save", HTTP_POST, []() {
    cfg.device_name = server.arg("devname");
    cfg.device_name.trim();

  float close = server.arg("close").toFloat();
  if (close < 1.0f) close = 1.0f;
  if (close > 60.0f) close = 60.0f;
  cfg.close_sec = close;

  int pause = server.arg("pause").toInt();
  if (pause < 0) pause = 0;
  if (pause > 600) pause = 600;
  cfg.pause_sec = pause;


    cfg.mqtt_server = server.arg("mqtt_srv");
    cfg.mqtt_user   = server.arg("mqtt_user");

    int p = server.arg("mqtt_port").toInt();
    cfg.mqtt_port = (p > 0 && p < 65536) ? p : 1883;

    String mp = server.arg("mqtt_pass");
    if (mp.length() > 0) cfg.mqtt_pass = mp;

    cfg.shelly_ip = server.arg("shelly_ip");
    cfg.shelly_ip.trim();

    cfg.shelly_ch = server.arg("shelly_ch").toInt();
    if (cfg.shelly_ch < 0) cfg.shelly_ch = 0;


    int p1 = server.arg("p1").toInt(); if (p1 < 1) p1 = 1; if (p1 > 600) p1 = 600;
    int p2 = server.arg("p2").toInt(); if (p2 < 1) p2 = 1; if (p2 > 600) p2 = 600;
    int p3 = server.arg("p3").toInt(); if (p3 < 1) p3 = 1; if (p3 > 600) p3 = 600;
    int p4 = server.arg("p4").toInt(); if (p4 < 1) p4 = 1; if (p4 > 600) p4 = 600;
    cfg.preset_min[0]=p1; cfg.preset_min[1]=p2; cfg.preset_min[2]=p3; cfg.preset_min[3]=p4;

    int sel = server.arg("psel").toInt(); // 1..4
    sel -= 1;
    if (sel < 0) sel = 0;
    if (sel > 3) sel = 3;
    cfg.preset_sel = sel;

    if (!cycleRunning) cfg.vent_min = cfg.preset_min[cfg.preset_sel];

    saveSettings();

    server.sendHeader("Location", "/settings");
    server.send(303);
    lastMqttRetryMs = 0;
  });

  // WiFi speichern
server.on("/wifi", HTTP_POST, []() {

  // ===== WLAN-Modus (v3.0) =====
  //int wm = server.arg("wifi_mode").toInt();
  //if (wm < 0 || wm > 2) wm = WIFI_AUTO;
  //cfg.wifi_mode = wm;

int wm = server.hasArg("wifi_mode")
          ? server.arg("wifi_mode").toInt()
          : cfg.wifi_mode;

if (wm < WIFI_AP_ONLY || wm > WIFI_AUTO) wm = WIFI_AUTO;
cfg.wifi_mode = wm;   // ✅ WICHTIG: übernehmen
LOGI("WiFi mode set to %d", cfg.wifi_mode);

 

  // ===== STA-Daten =====
  String ssid = server.arg("ssid"); ssid.trim();
  String pass = server.arg("pass");

if (cfg.wifi_mode == WIFI_STA_ONLY || cfg.wifi_mode == WIFI_AUTO) {

  if (ssid.length() > 0) {
    // normale STA-Konfiguration
    bool ssidChanged = (ssid != cfg.wifi_ssid);
    if (ssidChanged && pass.length() == 0) {
      server.send(400, "text/plain",
                  "Fehler: Passwort erforderlich (SSID wurde geändert).");
      return;
    }
    if (pass.length() == 0) pass = cfg.wifi_pass;
    saveWiFi(ssid, pass);

  } else {
    // SSID leer → bewusst kein STA
    LOGI("STA disabled (SSID empty)");
    saveWiFi("", "");
  }

} else {
  // AP_ONLY
  LOGI("AP_ONLY: clearing STA credentials");
  saveWiFi("", "");
}

  // ===== IP-Konfiguration =====
  bool ipStatic = server.hasArg("ip_static");

  String ipAddr = server.arg("ip_addr"); ipAddr.trim();
  String ipGw   = server.arg("ip_gw");   ipGw.trim();
  String ipMask = server.arg("ip_mask"); ipMask.trim();
  String ipDns  = server.arg("ip_dns");  ipDns.trim();

  if (ipStatic) {
    IPAddress ip, gw, mask, dns;
    bool okIp   = ip.fromString(ipAddr);
    bool okGw   = gw.fromString(ipGw);
    bool okMask = mask.fromString(ipMask);
    bool okDns  = (ipDns.length() == 0) ? true : dns.fromString(ipDns);

    if (!(okIp && okGw && okMask && okDns)) {
      server.send(400, "text/plain", "Fehler: Ungueltige Static-IP Werte.");
      return;
    }

    cfg.ip_addr = ipAddr;
    cfg.ip_gw   = ipGw;
    cfg.ip_mask = ipMask;
    cfg.ip_dns  = ipDns;
  }
  cfg.ip_static = ipStatic;

  // ===== AP-Daten =====
  String apSsid = server.arg("ap_ssid"); apSsid.trim();
  cfg.ap_ssid = apSsid;

  String apPass = server.arg("ap_pass");
  if (apPass.length() > 0) {
    if (apPass.length() < 8) {
      server.send(400, "text/plain", "Fehler: AP Passwort muss min. 8 Zeichen haben.");
      return;
    }
    cfg.ap_pass = apPass;
  }

  saveSettings();

  server.send(200, "text/plain", "Gespeichert. Neustart in 2 Sekunden...");
  server.client().stop();
  delay(2000);
  ESP.restart();
});

  server.begin();
  LOGI("Webserver started");
}

// =======================================================
// Setup / Loop
// =======================================================
void setup() {
  Serial.begin(115200);
  delay(1200);

  Serial.println();
  Serial.println("==== Boot Garagenlueftung ====");
  Serial.printf("FW: %s (%s)\n", FW_VERSION, FW_DATE);
  pinMode(PIN_TOR_RELAIS, OUTPUT);
  pinMode(PIN_LUEFTER,    OUTPUT);

  writeOut(PIN_TOR_RELAIS, false, TOR_ACTIVE_LOW);
  writeOut(PIN_LUEFTER,    false, FAN_ACTIVE_LOW);
  
  pinMode(PIN_START_BTN, INPUT_PULLUP);
  pinMode(PIN_ABORT_BTN, INPUT_PULLUP);
  pinMode(PIN_MODE_BTN, INPUT_PULLUP);
  pinMode(PIN_SETUP_BTN, INPUT_PULLUP);

  LOGI("SetupPin GPIO%d raw=%d", PIN_SETUP_BTN, digitalRead(PIN_SETUP_BTN));

/*
  bool st;
  if (shellySwitchGet(SHELLY_IP, SHELLY_CH, st)) {
    LOGI("Shelly initial state = %s", st ? "ON" : "OFF");
  }
*/

  loadSettings();

  // MQTT Topics pro Geraet eindeutig
  generateMqttTopics();


  LOGI("Before uiInit");
  uiInit();
  LOGI("After uiInit");

  // Preset beim Boot anwenden
  if (!cycleRunning) cfg.vent_min = cfg.preset_min[cfg.preset_sel];

delay(30);

bool setupPressed = (digitalRead(PIN_SETUP_BTN) == LOW);
bool hasCreds     = staHasCreds();

LOGI("BOOT WiFi: mode=%d setup=%d creds=%d",
     cfg.wifi_mode,
     setupPressed,
     hasCreds);

switch (cfg.wifi_mode) {

  case WIFI_AP_ONLY:
    LOGW("WiFi mode: AP_ONLY");
    startSetupAPManaged(true);
    break;

  case WIFI_STA_ONLY:
    LOGW("WiFi mode: STA_ONLY");
    if (hasCreds && connectWiFi()) {
      LOGI("STA connected");
    } else {
      LOGE("STA_ONLY but no WiFi -> fallback AP");
      startSetupAPManaged(true);
    }
    break;

  case WIFI_AUTO:
  default:
    LOGW("WiFi mode: AUTO");
    if (setupPressed) {
      startSetupAPManaged(true);
    }
    else if (hasCreds && connectWiFi()) {
      LOGI("STA connected");
    }
    else {
      startSetupAPManaged(false);
    }
    break;
}



  setupWeb();
}

void loop() {

    // 🔑 AP priorisieren
  if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_MODE_APSTA) {
    server.handleClient();
    delay(1);
  }

  server.handleClient();
  loopCycle();

  // Buttons
  if (digitalRead(PIN_START_BTN) == LOW && millis() - lastBtnMs > 500) {
    lastBtnMs = millis();
    startCycle();
  }
  if (digitalRead(PIN_ABORT_BTN) == LOW && millis() - lastBtnMs > 500) {
    lastBtnMs = millis();
    abortCycle();
  }
  handleModeButton();

  // MQTT
  mqttEnsureConnected();
  if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
    mqtt.loop();
    mqttPublish(false);
  }



  // ===== AP Auto-Off (nur wenn nicht permanent) =====
  if (apRunning && !apPermanent) {
    if ((uint32_t)(millis() - apStartedMs) > AP_TIMEOUT_MS) {
      LOGW("AP timeout -> turning AP off");
      WiFi.softAPdisconnect(true);
      apRunning = false;

      // falls inzwischen Credentials gesetzt wurden, nochmal verbinden
      if (hasWifiCreds()) {
        LOGI("Trying WiFi after AP timeout...");
        connectWiFi();
      }
    }
  }

  // Display
  uiTick();

  // ---- Heartbeat (alle 5s) ----
  static unsigned long lastHbMs = 0;
  unsigned long now = millis();
  if (now - lastHbMs >= 5000UL) {
    lastHbMs = now;
    LOGI("HB up=%lus wifi=%s mode=%d ap=%d mqtt=%s state=%s preset=P%d vent=%d",
        now / 1000UL,
        wifiUsable() ? "USABLE" : "OFF",
        WiFi.getMode(),
        WiFi.softAPgetStationNum(),
        (mqtt.connected() ? "OK" : "OFF"),
        stateStr().c_str(),
        cfg.preset_sel + 1,
        cfg.vent_min);

  }
}
