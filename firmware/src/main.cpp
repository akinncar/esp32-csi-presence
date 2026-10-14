// WiFi CSI presence sensor.
//
// Joins your WiFi, pings the router ~100x/s and measures how the WiFi channel
// fluctuates (CSI). Movement in the room makes it fluctuate more.
//
//  - RGB LED: red = someone present, off = empty,
//             blinking yellow = leave the room (calibration countdown),
//             blinking blue = calibrating.
//  - BOOT button: recalibrate the empty room.
//  - Dashboard: http://presence.local/ (or the IP printed on serial).
//  - Serial: send 'S' to stream raw samples as  CSI,<ms>,<rssi>,<amp0>,...,<amp63>
//            (used by presence.py --log); 'X' stops. Lines starting with '#' are status.
//
// WiFi credentials: built in from WIFI_SSID / WIFI_PASS env vars (see wifi_env.py),
// then saved on the board so later builds without them still connect.
// Without credentials it falls back to passive listening (few samples/s, no dashboard).

#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "detector.h"
#include "esp_wifi.h"
#include "ping/ping_sock.h"
#include "web_ui.h"

#ifndef WIFI_SSID_BYTES
#define WIFI_SSID_BYTES 0
#endif
#ifndef WIFI_PASS_BYTES
#define WIFI_PASS_BYTES 0
#endif

// Filled in by wifi_env.py from the WIFI_SSID / WIFI_PASS environment variables.
static const char BUILD_SSID[] = {WIFI_SSID_BYTES};
static const char BUILD_PASS[] = {WIFI_PASS_BYTES};

static const int PING_INTERVAL_MS = 10;  // ~100 samples/s
static const int BOOT_BUTTON = 0;
static const char *HOSTNAME = "presence";

struct CsiSample {
  uint32_t ms;
  int8_t rssi;
  uint8_t amp[PresenceDetector::N_SUB];
};

static QueueHandle_t csiQueue;
static uint8_t bssid[6];
static int channel = 1;
static volatile int8_t lastRssi = 0;
static uint32_t csiCount = 0, csiDropped = 0;
static bool connected = false;
static bool streamSerial = false;
static String ssid, pass;

static PresenceDetector detector;
static WebServer server(80);

// ---------- CSI capture ----------

static void csiCallback(void *ctx, wifi_csi_info_t *info) {
  if (!info || !info->buf || memcmp(info->mac, bssid, 6) != 0) return;
  CsiSample s;
  s.ms = millis();
  s.rssi = info->rx_ctrl.rssi;
  lastRssi = s.rssi;
  int n = min((int)info->len / 2, PresenceDetector::N_SUB);
  for (int i = 0; i < PresenceDetector::N_SUB; i++) {
    if (i < n) {
      int im = info->buf[2 * i];
      int re = info->buf[2 * i + 1];
      float a = sqrtf((float)(re * re + im * im));
      s.amp[i] = a > 255 ? 255 : (uint8_t)a;
    } else {
      s.amp[i] = 0;
    }
  }
  csiCount++;
  if (xQueueSend(csiQueue, &s, 0) != pdTRUE) csiDropped++;  // never block the WiFi task
}

static void enableCsi() {
  wifi_csi_config_t cfg = {};
  cfg.lltf_en = true;
  cfg.htltf_en = false;
  cfg.stbc_htltf2_en = false;
  cfg.ltf_merge_en = true;
  cfg.channel_filter_en = false;
  cfg.manu_scale = false;
  cfg.shift = 0;
  esp_wifi_set_csi_config(&cfg);
  esp_wifi_set_csi_rx_cb(csiCallback, nullptr);
  esp_wifi_set_csi(true);
}

// ---------- WiFi ----------

static void loadCredentials() {
  Preferences p;
  p.begin("wifi", false);
  if (strlen(BUILD_SSID) > 0 && strlen(BUILD_PASS) > 0) {
    ssid = BUILD_SSID;
    pass = BUILD_PASS;
  } else {
    ssid = p.getString("ssid", strlen(BUILD_SSID) ? BUILD_SSID : "");
    pass = p.getString("pass", "");
  }
  p.end();
}

static void saveCredentials() {
  Preferences p;
  p.begin("wifi", false);
  if (p.getString("ssid", "") != ssid) p.putString("ssid", ssid);
  if (p.getString("pass", "") != pass) p.putString("pass", pass);
  p.end();
}

static const char *disconnectReason(uint8_t r) {
  switch (r) {
    case 2: case 202: return "authentication failed (often wrong password)";
    case 15: case 204: return "handshake timeout (usually wrong password)";
    case 201: return "network not found";
    case 203: return "association failed";
    case 205: return "connection failed";
    default: return "other";
  }
}

static bool startConnected() {
  Serial.printf("# Connecting to %s ...\n", ssid.c_str());
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    uint8_t r = info.wifi_sta_disconnected.reason;
    Serial.printf("# WiFi disconnected: reason %u = %s\n", r, disconnectReason(r));
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  // Configure the connection directly so WPA3 routers that require SAE hash-to-element work.
  wifi_config_t conf = {};
  strncpy((char *)conf.sta.ssid, ssid.c_str(), sizeof(conf.sta.ssid));
  strncpy((char *)conf.sta.password, pass.c_str(), sizeof(conf.sta.password));
  conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  conf.sta.pmf_cfg.capable = true;
  conf.sta.pmf_cfg.required = false;
  conf.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
  conf.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  esp_wifi_set_config(WIFI_IF_STA, &conf);
  esp_wifi_connect();
  for (int i = 0; i < 80 && WiFi.status() != WL_CONNECTED; i++) delay(250);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("# Could not connect; falling back to passive");
    WiFi.disconnect();
    return false;
  }
  saveCredentials();
  WiFi.setSleep(false);  // power save would delay ping replies
  memcpy(bssid, WiFi.BSSID(), 6);
  channel = WiFi.channel();
  Serial.printf("# Connected: ch%d  %s  %d dBm  ip=%s\n", channel, WiFi.BSSIDstr().c_str(),
                WiFi.RSSI(), WiFi.localIP().toString().c_str());

  enableCsi();

  esp_ping_config_t pingCfg = ESP_PING_DEFAULT_CONFIG();
  ip_addr_t gw;
  IP_ADDR4(&gw, WiFi.gatewayIP()[0], WiFi.gatewayIP()[1], WiFi.gatewayIP()[2], WiFi.gatewayIP()[3]);
  pingCfg.target_addr = gw;
  pingCfg.count = ESP_PING_COUNT_INFINITE;
  pingCfg.interval_ms = PING_INTERVAL_MS;
  pingCfg.timeout_ms = 1000;
  pingCfg.data_size = 1;
  pingCfg.task_stack_size = 4096;
  esp_ping_callbacks_t cbs = {};
  esp_ping_handle_t ping;
  esp_ping_new_session(&pingCfg, &cbs, &ping);
  esp_ping_start(ping);
  return true;
}

static void startPassive() {
  Serial.println("# Passive mode: scanning for WiFi networks...");
  int best = -1;
  while (best < 0) {
    int n = WiFi.scanNetworks(false, true);
    for (int i = 0; i < n; i++) {
      bool match = ssid.length() == 0 || WiFi.SSID(i) == ssid;
      if (match && (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best))) best = i;
    }
    if (best < 0) delay(3000);
  }
  memcpy(bssid, WiFi.BSSID(best), 6);
  channel = WiFi.channel(best);
  Serial.printf("# Listening to: %s  ch%d  %s\n", WiFi.SSID(best).c_str(), channel,
                WiFi.BSSIDstr(best).c_str());
  WiFi.scanDelete();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  enableCsi();
}

// ---------- Dashboard ----------

static void handleState() {
  uint32_t now = millis();
  const char *calib = detector.calib == PresenceDetector::CALIB_COUNTDOWN    ? "countdown"
                      : detector.calib == PresenceDetector::CALIB_RECORDING ? "recording"
                                                                             : "idle";
  char json[400];
  char score[16] = "null", last[16] = "null";
  if (detector.hasScore) snprintf(score, sizeof(score), "%.2f", detector.score);
  if (detector.lastMotion) snprintf(last, sizeof(last), "%.1f", (now - detector.lastMotion) / 1000.0f);
  snprintf(json, sizeof(json),
           "{\"present\":%s,\"score\":%s,\"threshold\":%.2f,\"baseline\":%.2f,\"margin\":%.2f,"
           "\"rate\":%.1f,\"rssi\":%d,\"lastMotionAgo\":%s,\"calib\":\"%s\",\"calibRemaining\":%.1f,"
           "\"uptime\":%lu,\"channel\":%d}",
           detector.present ? "true" : "false", score, detector.threshold(), detector.baseline,
           detector.margin, detector.rate, lastRssi, last, calib,
           detector.calibRemainingMs(now) / 1000.0f, (unsigned long)(now / 1000), channel);
  server.send(200, "application/json", json);
}

static void startDashboard() {
  server.on("/", [] { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api/state", handleState);
  server.on("/api/calibrate", HTTP_POST, [] {
    detector.startCalibration();
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/margin", HTTP_POST, [] {
    if (server.hasArg("v")) detector.setMargin(server.arg("v").toFloat());
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.begin();
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
  Serial.printf("# Dashboard: http://%s.local/  or  http://%s/\n", HOSTNAME,
                WiFi.localIP().toString().c_str());
}

// ---------- LED and button ----------

static void setLed(uint8_t r, uint8_t g, uint8_t b) {
  static uint32_t last = 0xffffffff;
  uint32_t c = (r << 16) | (g << 8) | b;
  if (c == last) return;
  last = c;
  neopixelWrite(RGB_BUILTIN, r, g, b);
}

static void updateLed(uint32_t now) {
  bool blinkOn = (now / 400) % 2 == 0;
  switch (detector.calib) {
    case PresenceDetector::CALIB_COUNTDOWN: blinkOn ? setLed(60, 40, 0) : setLed(0, 0, 0); break;
    case PresenceDetector::CALIB_RECORDING: blinkOn ? setLed(0, 0, 60) : setLed(0, 0, 0); break;
    default: detector.present ? setLed(80, 0, 0) : setLed(0, 0, 0);
  }
}

static void checkButton(uint32_t now) {
  static bool wasDown = false;
  static uint32_t downSince = 0;
  bool down = digitalRead(BOOT_BUTTON) == LOW;
  if (down && !wasDown) downSince = now;
  if (!down && wasDown && now - downSince > 50) {
    Serial.println("# BOOT pressed: recalibrating");
    detector.startCalibration();
  }
  wasDown = down;
}

static void checkSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'S') streamSerial = true;
    if (c == 'X') streamSerial = false;
    if (c == 'C') detector.startCalibration();
  }
}

// ---------- Main ----------

void setup() {
  Serial.begin(460800);
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  setLed(0, 0, 0);
  delay(300);
  csiQueue = xQueueCreate(128, sizeof(CsiSample));

  detector.begin();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.disconnect();
  loadCredentials();

  connected = ssid.length() > 0 && pass.length() > 0 && startConnected();
  if (connected) startDashboard();
  else startPassive();
  Serial.println("# READY");
}

void loop() {
  uint32_t now = millis();

  CsiSample s;
  while (xQueueReceive(csiQueue, &s, 0) == pdTRUE) {
    detector.addSample(s.ms, s.amp);
    if (streamSerial) {
      char line[400];
      int p = snprintf(line, sizeof(line), "CSI,%lu,%d", (unsigned long)s.ms, s.rssi);
      for (int i = 0; i < PresenceDetector::N_SUB; i++)
        p += snprintf(line + p, sizeof(line) - p, ",%u", s.amp[i]);
      Serial.println(line);
    }
  }

  static uint32_t lastUpdate = 0;
  if (now - lastUpdate >= 200) {
    lastUpdate = now;
    bool wasPresent = detector.present;
    detector.update(now);
    if (detector.present != wasPresent)
      Serial.printf("# presence: %s\n", detector.present ? "PRESENT" : "EMPTY");
  }

  static uint32_t lastStats = 0;
  if (now - lastStats >= 5000) {
    lastStats = now;
    Serial.printf("# stats: csi=%lu dropped=%lu rate=%.0f/s score=%.2f threshold=%.2f %s\n",
                  (unsigned long)csiCount, (unsigned long)csiDropped, detector.rate,
                  detector.score, detector.threshold(),
                  detector.calib != PresenceDetector::CALIB_IDLE ? "(calibrating)"
                  : detector.present                              ? "PRESENT"
                                                                  : "EMPTY");
  }

  updateLed(now);
  checkButton(now);
  checkSerial();
  if (connected) server.handleClient();
  delay(1);
}
