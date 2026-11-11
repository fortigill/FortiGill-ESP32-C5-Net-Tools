// ESP32-C5 Net Tools - built fresh for the Waveshare ESP32-C5-Zero.
// Tool #2: WiFi connectivity foundation (STA + AP fallback + captive
// portal + status banner + LED status). Everything else gets added as
// its own tab on top of this.

#include <WiFi.h>
#include <WiFiUdp.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include "esp_wifi_ap_get_sta_list.h"
#include <Preferences.h>
#define MQTT_MAX_PACKET_SIZE 2048
#include <PubSubClient.h>
#include <Adafruit_NeoPixel.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/icmp.h"
#include "lwip/ip.h"
#include "lwip/inet_chksum.h"
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#define FIRMWARE_VERSION "v2.12.1"

// ---- RGB LED (WS2812 on GPIO27 - confirmed via color-cycle test) ----
#define RGB_LED_PIN 27
Adafruit_NeoPixel rgbLed(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);
bool rgbLedOn = true;
unsigned long lastLedBlinkMs = 0;
bool ledBlinkState = false;
bool rememberLedPref = false;
bool band2gOnly = false;
bool connectedVia5g = false;
bool fellBackTo2g = false;
unsigned long lastConnectedMs = 0;
const unsigned long BAND_FALLBACK_TIMEOUT_MS = 20000;  // how long disconnected before giving up on 5GHz
bool useStaticIp = false;
String staticIp = "";
bool meshModeEnabled = false;
bool bleScheduleActive = false;
unsigned long bleScheduleIntervalMs = 0;
unsigned long bleScheduleNextRunMs = 0;
bool ridScheduleActive = false;
unsigned long ridScheduleIntervalMs = 0;
unsigned long ridScheduleNextRunMs = 0;
String meshNodeId = "";
String meshName = "";
String meshPassword = "";
String meshServerAddr = "";
bool meshServerConnected = false;
WiFiClient meshWifiClient;
PubSubClient meshMqttClient(meshWifiClient);
unsigned long meshLastReconnectAttemptMs = 0;
String staticSubnet = "";
String staticGateway = "";
String staticDns1 = "";
String staticDns2 = "";
bool bleScanTaskRunning = false;
String bleScanTaskResult = "";
String bleScanDevicesJson = "[]";

#define BLE_GENERIC_MAX_DEVICES 40
struct BleGenericDevice {
  String mac;
  String name;
  int rssi;
  int companyId;
  int appleAdvType;  // byte after Apple's company ID in 0xFF manufacturer data; -1 = not Apple / not captured
  String mfgDataHex;  // raw 0xFF manufacturer-specific data (company ID + payload) as hex, for diagnostics
  int appearance;  // GAP Appearance AD field (0x19), standards-based device-category hint; -1 = not present
  bool hasFastPair;
  bool hasHeartRate;
  bool hasHid;
  bool isDrone;
  unsigned long lastSeenMs;
};
BleGenericDevice bleGenericDevices[BLE_GENERIC_MAX_DEVICES];
int bleGenericDeviceCount = 0;

int bleGenericFindOrAddDevice(const String &mac) {
  for (int i = 0; i < bleGenericDeviceCount; i++) {
    if (bleGenericDevices[i].mac.equalsIgnoreCase(mac)) return i;
  }
  if (bleGenericDeviceCount < BLE_GENERIC_MAX_DEVICES) {
    bleGenericDevices[bleGenericDeviceCount].mac = mac;
    bleGenericDevices[bleGenericDeviceCount].name = "";
    bleGenericDevices[bleGenericDeviceCount].rssi = 0;
    bleGenericDevices[bleGenericDeviceCount].companyId = -1;
    bleGenericDevices[bleGenericDeviceCount].appleAdvType = -1;
    bleGenericDevices[bleGenericDeviceCount].mfgDataHex = "";
    bleGenericDevices[bleGenericDeviceCount].appearance = -1;
    bleGenericDevices[bleGenericDeviceCount].hasFastPair = false;
    bleGenericDevices[bleGenericDeviceCount].hasHeartRate = false;
    bleGenericDevices[bleGenericDeviceCount].hasHid = false;
    bleGenericDevices[bleGenericDeviceCount].isDrone = false;
    bleGenericDevices[bleGenericDeviceCount].lastSeenMs = 0;
    bleGenericDeviceCount++;
    return bleGenericDeviceCount - 1;
  }
  return -1;
}
bool traceTaskRunning = false;
String traceTaskResult = "";
String traceTargetInput = "";
int traceMaxHopsInput = 30;
int traceTimeoutInput = 2000;
bool dnsLookupRunning = false;
String dnsLookupResult = "";
String dnsLookupInput = "";
bool whoisTaskRunning = false;
String whoisTaskResult = "";
String whoisInput = "";
bool reverseDnsRunning = false;
String reverseDnsResult = "";
String reverseDnsInput = "";
bool radarActive = false;
String radarTargetMac = "";
int radarLastRssi = -127;
unsigned long radarLastSeenMs = 0;

bool wifiScanTaskRunning = false;
String wifiScanTaskResult = "Choose a band and press Scan.";
int wifiScanRequestedBand = 0;  // 0 = 2.4GHz, 1 = 5GHz

const int CHANNELS_2G[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
const int NUM_CHANNELS_2G = 13;
const int CHANNELS_5G[] = {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165};
const int NUM_CHANNELS_5G = 25;

String jsonEscapeForWifiScan(const String &in) {
  String out = "";
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"') { out += "\\\""; }
    else if (c == '\\') { out += "\\\\"; }
    else if (c == '\n') { out += "\\n"; }
    else if (c == '\r') { /* skip */ }
    else if ((uint8_t)c < 0x20) { /* skip other control chars */ }
    else { out += c; }
  }
  return out;
}

String wifiStandardsStr(wifi_ap_record_t *r) {
  String s = "";
  if (r->phy_11b) s += "b/";
  if (r->phy_11g) s += "g/";
  if (r->phy_11n) s += "n/";
  if (r->phy_11a) s += "a/";
  if (r->phy_11ac) s += "ac/";
  if (r->phy_11ax) s += "ax/";
  if (s.length() > 0) s.remove(s.length() - 1);
  if (s.length() == 0) s = "?";
  return s;
}

String wifiBandwidthStr(wifi_bandwidth_t bw) {
  switch (bw) {
    case WIFI_BW_HT20: return "20MHz";
    case WIFI_BW_HT40: return "40MHz";
    case WIFI_BW80: return "80MHz";
    case WIFI_BW160: return "160MHz";
    case WIFI_BW80_BW80: return "80+80MHz";
    default: return "?";
  }
}

String wifiAuthStr(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN: return "Open (no password)";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK: return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3-PSK";
    case WIFI_AUTH_WAPI_PSK: return "WAPI-PSK";
    case WIFI_AUTH_OWE: return "OWE (enhanced open)";
    case WIFI_AUTH_WPA3_ENT_192: return "WPA3-Enterprise 192-bit";
    case WIFI_AUTH_DPP: return "DPP";
    case WIFI_AUTH_WPA3_ENTERPRISE: return "WPA3-Enterprise";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "WPA2/WPA3-Enterprise";
    case WIFI_AUTH_WPA_ENTERPRISE: return "WPA-Enterprise";
    default: return "Unknown";
  }
}

bool looksLikeFactoryDefault(const String &ssid) {
  String s = ssid;
  s.toUpperCase();
  const char *patterns[] = {
    "NETGEAR", "ASUS", "TP-LINK", "TPLINK", "DLINK", "D-LINK", "LINKSYS",
    "TENDA", "BELKIN", "TRENDNET", "ZYXEL", "HUAWEI", "MIKROTIK",
    "XFINITY", "SPECTRUM SETUP", "ATTWIFI", "CENTURYLINK", "VERIZON_",
    "ARRIS", "UBEE", "MERCKU", "DEFAULT", "SETUP", "UNCONFIGURED",
    "MOTOROLA", "ACTIONTEC", "FRONTIER"
  };
  for (unsigned int i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
    if (s.indexOf(patterns[i]) >= 0) return true;
  }
  return false;
}

bool looksLikeRepeater(const String &ssid) {
  String s = ssid;
  s.toUpperCase();
  const char *patterns[] = {
    "_EXT", "-EXT", "EXTENDER", "REPEATER", "_RPT", "-RPT", "_RE_", "-RE_",
    "RANGE EXTENDER"
  };
  for (unsigned int i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
    if (s.indexOf(patterns[i]) >= 0) return true;
  }
  return false;
}

// ---- Ping tool ----
bool pingTaskRunning = false;
String pingTaskResult = "";
String pingTargetInput = "";
int pingTtlInput = 64;
int pingSizeInput = 64;
bool pingContinuousMode = false;
volatile bool pingStopRequested = false;

static String pingResultLines = "";
static uint32_t pingMinMs = 0xFFFFFFFF;
static uint32_t pingMaxMs = 0;
static uint64_t pingSumMs = 0;
static volatile bool pingSessionDone = false;
static volatile uint32_t pingReceivedCount = 0;
static volatile uint32_t pingLastElapsedMs = 0;

static void onPingSuccess(esp_ping_handle_t hdl, void *args) {
  uint16_t seqno;
  uint8_t ttl;
  uint32_t recvLen, elapsedMs;
  ip_addr_t addr;
  esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
  esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
  esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &recvLen, sizeof(recvLen));
  esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsedMs, sizeof(elapsedMs));
  esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &addr, sizeof(addr));
  char ipStr[16];
  ip4addr_ntoa_r(ip_2_ip4(&addr), ipStr, sizeof(ipStr));
  pingResultLines += "Reply from " + String(ipStr) + ": bytes=" + String(recvLen) +
                      " ttl=" + String(ttl) + " time=" + String(elapsedMs) + "ms seq=" + String(seqno) + "\n";
  pingTaskResult = pingResultLines;
  if (elapsedMs < pingMinMs) pingMinMs = elapsedMs;
  if (elapsedMs > pingMaxMs) pingMaxMs = elapsedMs;
  pingSumMs += elapsedMs;
  pingReceivedCount++;
  pingLastElapsedMs = elapsedMs;
}

static void onPingTimeout(esp_ping_handle_t hdl, void *args) {
  uint16_t seqno;
  esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
  pingResultLines += "Request timeout (seq=" + String(seqno) + ")\n";
  pingTaskResult = pingResultLines;
}

static void onPingEnd(esp_ping_handle_t hdl, void *args) {
  uint32_t transmitted, received, totalTimeMs;
  esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
  esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
  esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &totalTimeMs, sizeof(totalTimeMs));
  float lossPct = (transmitted > 0) ? (100.0f * (float)(transmitted - received) / (float)transmitted) : 0.0f;
  pingResultLines += "\n--- ping statistics ---\n";
  pingResultLines += String(transmitted) + " packets transmitted, " + String(received) +
                      " received, " + String(lossPct, 1) + "% packet loss, time " + String(totalTimeMs) + "ms\n";
  if (received > 0) {
    float avgMs = (float)pingSumMs / (float)received;
    pingResultLines += "rtt min/avg/max = " + String(pingMinMs) + "/" + String(avgMs, 1) + "/" + String(pingMaxMs) + " ms\n";
  }
  pingTaskResult = pingResultLines;
  pingSessionDone = true;
}

void pingTask(void *param) {
  String target = pingTargetInput;
  int ttl = pingTtlInput;
  int size = pingSizeInput;

  pingResultLines = "";
  pingMinMs = 0xFFFFFFFF;
  pingMaxMs = 0;
  pingSumMs = 0;
  pingSessionDone = false;
  pingStopRequested = false;
  pingReceivedCount = 0;
  pingLastElapsedMs = 0;

  pingTaskResult = "Resolving " + target + "...";

  struct addrinfo hint;
  struct addrinfo *res = NULL;
  memset(&hint, 0, sizeof(hint));
  ip_addr_t targetAddr;
  memset(&targetAddr, 0, sizeof(targetAddr));

  int gaiErr = getaddrinfo(target.c_str(), NULL, &hint, &res);
  if (gaiErr != 0 || res == NULL) {
    pingTaskResult = "Could not resolve \"" + target + "\".";
    pingTaskRunning = false;
    vTaskDelete(NULL);
    return;
  }
  struct in_addr addr4 = ((struct sockaddr_in *)(res->ai_addr))->sin_addr;
  inet_addr_to_ip4addr(ip_2_ip4(&targetAddr), &addr4);
  IP_SET_TYPE_VAL(targetAddr, IPADDR_TYPE_V4);
  freeaddrinfo(res);

  char resolvedIpStr[16];
  ip4addr_ntoa_r(ip_2_ip4(&targetAddr), resolvedIpStr, sizeof(resolvedIpStr));
  pingResultLines = "Pinging " + target + " [" + String(resolvedIpStr) + "] with " + String(size) +
                     " bytes of data, TTL=" + String(ttl) + ":\n\n";
  pingTaskResult = pingResultLines;

  esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
  config.target_addr = targetAddr;
  config.count = pingContinuousMode ? 0 : 5;  // 0 = ping indefinitely
  config.data_size = size;
  config.ttl = ttl;
  config.interval_ms = 1000;
  config.timeout_ms = 1000;

  esp_ping_callbacks_t cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.on_ping_success = onPingSuccess;
  cbs.on_ping_timeout = onPingTimeout;
  cbs.on_ping_end = onPingEnd;

  esp_ping_handle_t pingHdl;
  if (esp_ping_new_session(&config, &cbs, &pingHdl) != ESP_OK) {
    pingTaskResult = "Failed to create ping session.";
    pingTaskRunning = false;
    vTaskDelete(NULL);
    return;
  }

  esp_ping_start(pingHdl);

  if (pingContinuousMode) {
    while (!pingStopRequested) {
      delay(100);
    }
  } else {
    unsigned long startWait = millis();
    while (!pingSessionDone && (millis() - startWait) < 15000) {
      delay(100);
    }
  }

  esp_ping_stop(pingHdl);
  esp_ping_delete_session(pingHdl);

  pingTaskRunning = false;
  vTaskDelete(NULL);
}

String formatOneScanResult(int rank, wifi_ap_record_t *r) {
  String ssid = String((char *)r->ssid);
  if (ssid.length() == 0) ssid = "<hidden>";
  String band = (r->primary <= 14) ? "2.4GHz" : "5GHz";
  char bssidStr[18];
  snprintf(bssidStr, sizeof(bssidStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           r->bssid[0], r->bssid[1], r->bssid[2], r->bssid[3], r->bssid[4], r->bssid[5]);
  String flags = "";
  if (r->phy_11ax) flags += " [WiFi 6]";
  if (r->wps) flags += " [WPS]";
  if (r->ftm_responder) flags += " [FTM-responder]";
  if (r->ftm_initiator) flags += " [FTM-initiator]";

  String out = "";
  out += String(rank) + ". " + ssid + flags + "\n";
  out += "   Channel " + String(r->primary) + " (" + band + ")\n";
  out += "   RSSI " + String(r->rssi) + " dBm\n";
  out += "   Security: " + wifiAuthStr(r->authmode) + "\n";
  out += "   Standards: 802.11" + wifiStandardsStr(r) + "\n";
  out += "   Bandwidth: " + wifiBandwidthStr(r->bandwidth) + "\n";
  out += "   BSSID: " + String(bssidStr) + "\n";
  out += "\n";
  return out;
}

void wifiScanTask(void *param) {
  bool is5g = (wifiScanRequestedBand == 1);
  String bandName = is5g ? "5GHz" : "2.4GHz";
  const int *channels = is5g ? CHANNELS_5G : CHANNELS_2G;
  int numChannels = is5g ? NUM_CHANNELS_5G : NUM_CHANNELS_2G;

  Serial.println("Scan task: setting band mode to " + bandName + "...");
  WiFi.setBandMode(is5g ? WIFI_BAND_MODE_5G_ONLY : WIFI_BAND_MODE_2G_ONLY);
  Serial.println("Scan task: band mode set, starting scan...");
  delay(150);

  const int MAX_RESULTS = 64;
  wifi_ap_record_t *allRecs = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * MAX_RESULTS);
  int totalFound = 0;
  unsigned long scanStartMs = millis();

  for (int ci = 0; ci < numChannels; ci++) {
    wifiScanTaskResult = "Scanning " + bandName + ": channel " + String(channels[ci]) +
                         " (" + String(ci + 1) + " of " + String(numChannels) + ")";
    Serial.println("Scan task: channel " + String(channels[ci]) + " (" + String(ci + 1) + "/" + String(numChannels) +
                    ") - " + String((millis() - scanStartMs) / 1000) + "s elapsed");

    int n = WiFi.scanNetworks(false, true, is5g, 300, channels[ci]);
    if (n > 0) {
      for (int i = 0; i < n && totalFound < MAX_RESULTS; i++) {
        allRecs[totalFound++] = *(wifi_ap_record_t *)WiFi.getScanInfoByIndex(i);
      }
    }
    WiFi.scanDelete();
  }

  Serial.println("Scan task: scanned " + String(numChannels) + " channels, found " + String(totalFound) + " network(s)");

  for (int i = 1; i < totalFound; i++) {
    wifi_ap_record_t key = allRecs[i];
    int j = i - 1;
    while (j >= 0 && allRecs[j].rssi < key.rssi) {
      allRecs[j + 1] = allRecs[j];
      j--;
    }
    allRecs[j + 1] = key;
  }

  String result;
  if (totalFound == 0) {
    result = "No networks found on " + bandName + ".";
  } else {
    result = "Found " + String(totalFound) + " network(s) on " + bandName + " (scanned " + String(numChannels) + " channels).\n\n";
    for (int i = 0; i < totalFound; i++) {
      result += formatOneScanResult(i + 1, &allRecs[i]);
    }

    String meshSection = "";
    String standaloneSection = "";
    String defaultSection = "";
    String repeaterSection = "";
    bool grouped[MAX_RESULTS];
    for (int i = 0; i < totalFound; i++) grouped[i] = false;

    for (int i = 0; i < totalFound; i++) {
      if (grouped[i]) continue;
      String ssidI = String((char *)allRecs[i].ssid);
      grouped[i] = true;
      if (ssidI.length() == 0) {
        standaloneSection += "  - <hidden> (channel " + String(allRecs[i].primary) + ")\n";
        continue;
      }
      String channelsStr = String(allRecs[i].primary);
      int count = 1;
      for (int j = i + 1; j < totalFound; j++) {
        if (grouped[j]) continue;
        String ssidJ = String((char *)allRecs[j].ssid);
        if (ssidJ == ssidI) {
          grouped[j] = true;
          count++;
          channelsStr += ", " + String(allRecs[j].primary);
        }
      }
      if (count > 1) {
        meshSection += "  - " + ssidI + " (" + String(count) + " access points) - channels " + channelsStr + "\n";
      } else {
        standaloneSection += "  - " + ssidI + " (channel " + String(allRecs[i].primary) + ")\n";
      }
      if (looksLikeFactoryDefault(ssidI)) {
        defaultSection += "  - " + ssidI + " (channel " + String(allRecs[i].primary) + ")\n";
      }
      if (looksLikeRepeater(ssidI)) {
        repeaterSection += "  - " + ssidI + " (channel " + String(allRecs[i].primary) + ")\n";
      }
    }

    result += "\n--- Network Summary ---\n\n";
    if (meshSection.length() > 0) {
      result += "Mesh / Multi-AP Networks (same SSID from multiple access points):\n" + meshSection + "\n";
    }
    if (standaloneSection.length() > 0) {
      result += "Standalone Networks:\n" + standaloneSection + "\n";
    }
    if (defaultSection.length() > 0) {
      result += "[!] Possible Factory-Default / Unconfigured Networks (name-pattern heuristic, not certain):\n" + defaultSection + "\n";
    }
    if (repeaterSection.length() > 0) {
      result += "[~] Possible Repeater / Range Extender Networks (name-pattern heuristic, not certain):\n" + repeaterSection + "\n";
    }
  }
  free(allRecs);

  Serial.println("Scan task: restoring AUTO band mode...");
  WiFi.setBandMode(WIFI_BAND_MODE_AUTO);
  Serial.println("Scan task: done.");

  wifiScanTaskResult = result;
  wifiScanTaskRunning = false;
  vTaskDelete(NULL);
}

DNSServer dnsServer;
bool isApMode = false;

struct APClientInfo { uint8_t mac[6]; unsigned long connectedAt; bool used; };
APClientInfo apClients[10];

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    for (int i = 0; i < 10; i++) {
      if (!apClients[i].used) {
        memcpy(apClients[i].mac, info.wifi_ap_staconnected.mac, 6);
        apClients[i].connectedAt = millis();
        apClients[i].used = true;
        break;
      }
    }
  } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    for (int i = 0; i < 10; i++) {
      if (apClients[i].used && memcmp(apClients[i].mac, info.wifi_ap_stadisconnected.mac, 6) == 0) {
        apClients[i].used = false;
        break;
      }
    }
  }
}

int countApClients() {
  int count = 0;
  for (int i = 0; i < 10; i++) if (apClients[i].used) count++;
  return count;
}

unsigned long lastBandBlinkMs = 0;
int bandBlinkStep = 0;
unsigned long bandBlinkStepStart = 0;

void updateStatusLed() {
  if (!rgbLedOn) {
    rgbLed.setPixelColor(0, 0);
    rgbLed.show();
    return;
  }

  if (wifiScanTaskRunning) {
    unsigned long now = millis();
    if (now - lastLedBlinkMs > 100) {
      lastLedBlinkMs = now;
      ledBlinkState = !ledBlinkState;
    }
    rgbLed.setPixelColor(0, ledBlinkState ? rgbLed.Color(0, 60, 0) : 0);  // fast blink green: scanning
    rgbLed.show();
    return;
  }

  extern bool pcapCaptureActive;
  if (pcapCaptureActive) {
    unsigned long now = millis();
    if (now - lastLedBlinkMs > 100) {
      lastLedBlinkMs = now;
      ledBlinkState = !ledBlinkState;
    }
    rgbLed.setPixelColor(0, ledBlinkState ? rgbLed.Color(0, 0, 60) : rgbLed.Color(60, 0, 0));  // fast cycle blue/red: packet capture active
    rgbLed.show();
    return;
  }

  extern bool ridScanActive;
  if (ridScanActive) {
    unsigned long now = millis();
    if (now - lastLedBlinkMs > 100) {
      lastLedBlinkMs = now;
      ledBlinkState = !ledBlinkState;
    }
    rgbLed.setPixelColor(0, ledBlinkState ? rgbLed.Color(40, 0, 60) : 0);  // fast blink purple: drone Remote ID scanning
    rgbLed.show();
    return;
  }

  if (bleScanTaskRunning || radarActive) {
    unsigned long now = millis();
    if (now - lastLedBlinkMs > 100) {
      lastLedBlinkMs = now;
      ledBlinkState = !ledBlinkState;
    }
    rgbLed.setPixelColor(0, ledBlinkState ? rgbLed.Color(0, 0, 60) : 0);  // fast blink blue: BLE scanning/tracking
    rgbLed.show();
    return;
  }

  extern bool portScanRunning;
  if (portScanRunning) {
    unsigned long now = millis();
    if (now - lastLedBlinkMs > 100) {
      lastLedBlinkMs = now;
      ledBlinkState = !ledBlinkState;
    }
    rgbLed.setPixelColor(0, ledBlinkState ? rgbLed.Color(60, 25, 0) : 0);  // fast blink orange: port scanning
    rgbLed.show();
    return;
  }

  if (isApMode) {
    int clients = countApClients();
    if (clients > 0) {
      rgbLed.setPixelColor(0, rgbLed.Color(0, 0, 60));  // solid blue: AP + client connected
    } else {
      unsigned long now = millis();
      if (now - lastLedBlinkMs > 500) {
        lastLedBlinkMs = now;
        ledBlinkState = !ledBlinkState;
      }
      rgbLed.setPixelColor(0, ledBlinkState ? rgbLed.Color(0, 0, 60) : 0);  // slow blink blue: AP, no client
    }
  } else {
    bool is5G = WiFi.channel() > 14;
    uint32_t bandColor = is5G ? rgbLed.Color(0, 0, 60) : rgbLed.Color(0, 60, 0);
    unsigned long now = millis();

    if (bandBlinkStep == 0 && now - lastBandBlinkMs >= 15000) {
      bandBlinkStep = 1;
      bandBlinkStepStart = now;
      lastBandBlinkMs = now;
    }

    if (bandBlinkStep == 0) {
      rgbLed.setPixelColor(0, rgbLed.Color(0, 60, 0));  // solid green: connected to home network
    } else if (bandBlinkStep <= 4) {
      if (now - bandBlinkStepStart >= 100) {
        bandBlinkStepStart = now;
        bandBlinkStep++;
        if (bandBlinkStep > 4) bandBlinkStep = meshModeEnabled ? 5 : 0;
      }
      bool blinkOn = (bandBlinkStep == 1 || bandBlinkStep == 3);
      rgbLed.setPixelColor(0, blinkOn ? bandColor : 0);  // every 15s: fast double-blink green=2.4GHz, blue=5GHz
    } else if (bandBlinkStep <= 8) {
      if (now - bandBlinkStepStart >= 300) {
        bandBlinkStepStart = now;
        bandBlinkStep++;
        if (bandBlinkStep > 8) bandBlinkStep = 9;
      }
      bool blinkOn = (bandBlinkStep == 5 || bandBlinkStep == 7);
      rgbLed.setPixelColor(0, blinkOn ? rgbLed.Color(60, 0, 0) : 0);  // mesh mode: 2 slow red heartbeat pulses, attempting to reach server
    } else {
      if (now - bandBlinkStepStart >= 300) {
        bandBlinkStepStart = now;
        bandBlinkStep++;
        if (bandBlinkStep > 10) bandBlinkStep = 0;
      }
      bool blinkOn = (bandBlinkStep == 9);
      uint32_t meshColor = meshServerConnected ? rgbLed.Color(40, 0, 60) : rgbLed.Color(60, 0, 0);
      rgbLed.setPixelColor(0, blinkOn ? meshColor : 0);  // mesh mode: 3rd pulse, purple if connected to server else slow red
    }
  }
  rgbLed.show();
}

Preferences preferences;
String savedSsid = "";
String savedPass = "";

AsyncWebServer server(80);

String doNetStatus() {
  String json = "{\"band2gOnly\":" + String(band2gOnly ? "true" : "false") + ",";
  if (WiFi.getMode() == WIFI_AP) {
    json += "\"mode\":\"ap\",\"apSsid\":\"FortiGill\",\"clients\":[";

    wifi_sta_list_t wifiStaList;
    esp_wifi_ap_get_sta_list(&wifiStaList);
    wifi_sta_mac_ip_list_t macIpList;
    esp_wifi_ap_get_sta_list_with_ip(&wifiStaList, &macIpList);

    bool first = true;
    for (int i = 0; i < macIpList.num; i++) {
      uint8_t* mac = macIpList.sta[i].mac;
      IPAddress clientIp(macIpList.sta[i].ip.addr);
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

      unsigned long connectedSec = 0;
      for (int j = 0; j < 10; j++) {
        if (apClients[j].used && memcmp(apClients[j].mac, mac, 6) == 0) {
          connectedSec = (millis() - apClients[j].connectedAt) / 1000;
          break;
        }
      }
      if (!first) json += ",";
      json += "{\"mac\":\"" + String(macStr) + "\",\"ip\":\"" + clientIp.toString() +
              "\",\"connectedSec\":" + String(connectedSec) + "}";
      first = false;
    }
    json += "]}";
  } else {
    String bandStr = (WiFi.getBand() == WIFI_BAND_5G) ? "5GHz" : "2.4GHz";
    String hostname = (meshModeEnabled && meshNodeId.length() > 0) ? ("esp32-c5-" + meshNodeId) : "esp32-c5";
    json += "\"mode\":\"sta\",\"ssid\":\"" + savedSsid + "\",\"band\":\"" + bandStr + "\",\"rssi\":" + String(WiFi.RSSI()) +
            ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"hostname\":\"" + hostname + "\"}";
  }
  return json;
}

// ===== PORT SCAN + OS GUESS TOOL =====
#include <lwip/sockets.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

struct PortInfo { uint16_t port; const char* name; };
static const PortInfo TCP_COMMON_PORTS[] = {
  {21, "FTP"}, {22, "SSH"}, {23, "Telnet"}, {25, "SMTP"}, {53, "DNS"},
  {80, "HTTP"}, {110, "POP3"}, {111, "RPCbind"}, {135, "MS-RPC"}, {139, "NetBIOS"},
  {143, "IMAP"}, {443, "HTTPS"}, {445, "SMB"}, {993, "IMAPS"}, {995, "POP3S"},
  {1723, "PPTP"}, {3306, "MySQL"}, {3389, "RDP"}, {5432, "PostgreSQL"}, {5900, "VNC"},
  {8080, "HTTP-alt"}, {8443, "HTTPS-alt"}, {8888, "HTTP-alt2"}, {9100, "Printer/JetDirect"}, {27017, "MongoDB"}
};
static const int TCP_COMMON_PORTS_COUNT = sizeof(TCP_COMMON_PORTS) / sizeof(PortInfo);

static const PortInfo UDP_COMMON_PORTS[] = {
  {53, "DNS"}, {67, "DHCP"}, {69, "TFTP"}, {123, "NTP"},
  {161, "SNMP"}, {500, "IKE/VPN"}, {514, "Syslog"}, {1900, "SSDP"}
};
static const int UDP_COMMON_PORTS_COUNT = sizeof(UDP_COMMON_PORTS) / sizeof(PortInfo);

bool portScanRunning = false;
String portScanResult = "";
String portScanTargetInput = "";
bool portScanSingleMode = false;
int portScanSinglePort = 0;
bool portScanSingleIsUdp = false;

bool icmpPingForTtl(IPAddress ip, int timeoutMs, int *ttlOut) {
  int sock = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
  if (sock < 0) return false;
  struct timeval tv;
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct sockaddr_in dest;
  memset(&dest, 0, sizeof(dest));
  dest.sin_family = AF_INET;
  dest.sin_addr.s_addr = (uint32_t)ip;
  struct icmp_echo_hdr req;
  memset(&req, 0, sizeof(req));
  ICMPH_TYPE_SET(&req, ICMP_ECHO);
  ICMPH_CODE_SET(&req, 0);
  req.chksum = 0;
  req.id = 0xAFAF;
  req.seqno = htons(1);
  req.chksum = inet_chksum(&req, sizeof(req));
  sendto(sock, &req, sizeof(req), 0, (struct sockaddr*)&dest, sizeof(dest));
  uint8_t buf[128];
  struct sockaddr_in from;
  socklen_t fromlen = sizeof(from);
  int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fromlen);
  close(sock);
  if (len <= 0) return false;
  struct ip_hdr *iphdr = (struct ip_hdr*)buf;
  int ihl = IPH_HL_BYTES(iphdr);
  if (len < ihl + (int)sizeof(struct icmp_echo_hdr)) return false;
  struct icmp_echo_hdr *icmp = (struct icmp_echo_hdr*)(buf + ihl);
  if (ICMPH_TYPE(icmp) != ICMP_ER) return false;
  if (ttlOut) *ttlOut = IPH_TTL(iphdr);
  return true;
}

bool tcpPortOpen(IPAddress ip, uint16_t port, int timeoutMs) {
  WiFiClient client;
  bool ok = client.connect(ip, port, timeoutMs);
  if (ok) client.stop();
  return ok;
}

String udpPortState(IPAddress ip, uint16_t port, int icmpSock, int timeoutMs) {
  WiFiUDP udp;
  udp.begin(0);
  udp.beginPacket(ip, port);
  uint8_t zero = 0;
  udp.write(&zero, 1);
  udp.endPacket();
  unsigned long start = millis();
  while (millis() - start < (unsigned long)timeoutMs) {
    uint8_t buf[128];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int len = recvfrom(icmpSock, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr*)&from, &fromlen);
    if (len > 0) {
      struct ip_hdr *outerIp = (struct ip_hdr*)buf;
      int ihl = IPH_HL_BYTES(outerIp);
      if (len >= ihl + (int)sizeof(struct icmp_echo_hdr)) {
        struct icmp_echo_hdr *icmp = (struct icmp_echo_hdr*)(buf + ihl);
        if (ICMPH_TYPE(icmp) == ICMP_DUR && ICMPH_CODE(icmp) == ICMP_DUR_PORT) {
          udp.stop();
          return "closed";
        }
      }
    }
    delay(20);
  }
  udp.stop();
  return "open|filtered";
}

String guessOsFromSignals(int ttl, bool sawTtl, bool rdpOpen, bool smbOpen, bool sshOpen, String banners) {
  String guess;
  if (!sawTtl) {
    guess = "Unknown (no ICMP reply to fingerprint)";
  } else if (ttl > 60 && ttl <= 64) {
    guess = "Likely Linux/Unix/macOS (TTL=" + String(ttl) + ", default ~64)";
  } else if (ttl > 110 && ttl <= 128) {
    guess = "Likely Windows (TTL=" + String(ttl) + ", default ~128)";
  } else if (ttl > 240 && ttl <= 255) {
    guess = "Likely network device/router (TTL=" + String(ttl) + ", default ~255)";
  } else {
    guess = "Unclear (TTL=" + String(ttl) + ")";
  }
  if (rdpOpen) guess += " - RDP (3389) open, strongly suggests Windows";
  else if (smbOpen) guess += " - SMB (445) open";
  if (sshOpen) guess += " - SSH (22) open, suggests Linux/Unix/macOS";

  String lowerBanners = banners;
  lowerBanners.toLowerCase();
  struct BannerHint { const char* keyword; const char* label; };
  static const BannerHint hints[] = {
    {"ubuntu", "Ubuntu Linux"}, {"debian", "Debian Linux"}, {"centos", "CentOS Linux"},
    {"red hat", "Red Hat Linux"}, {"fedora", "Fedora Linux"}, {"freebsd", "FreeBSD"},
    {"microsoft-iis", "Windows (IIS)"}, {"win32", "Windows"}, {"windows", "Windows"},
    {"dropbear", "Dropbear SSH (often an embedded Linux device/router)"},
    {"openssh", "OpenSSH (Linux/Unix/macOS/BSD)"},
  };
  for (int i = 0; i < (int)(sizeof(hints) / sizeof(BannerHint)); i++) {
    String kw = hints[i].keyword;
    if (lowerBanners.indexOf(kw) >= 0) {
      guess += " - banner suggests " + String(hints[i].label);
      break;
    }
  }
  return guess;
}

String grabSshBanner(IPAddress ip, int timeoutMs) {
  WiFiClient client;
  if (!client.connect(ip, 22, timeoutMs)) return "";
  unsigned long start = millis();
  String banner = "";
  while (millis() - start < (unsigned long)timeoutMs) {
    while (client.available()) {
      char c = client.read();
      if (c == '\n') { client.stop(); return banner; }
      if (c != '\r') banner += c;
    }
    if (banner.length() > 0 && !client.connected()) break;
    delay(10);
  }
  client.stop();
  return banner;
}

String grabHttpServerHeader(IPAddress ip, uint16_t port, bool useTls, int timeoutMs) {
  String response = "";
  if (useTls) {
    WiFiClientSecure client;
    client.setInsecure();
    if (!client.connect(ip, port, timeoutMs)) return "";
    client.print("GET / HTTP/1.1\r\nHost: " + ip.toString() + "\r\nConnection: close\r\n\r\n");
    unsigned long start = millis();
    while ((client.connected() || client.available()) && millis() - start < (unsigned long)timeoutMs) {
      while (client.available()) response += (char)client.read();
      if (response.indexOf("\r\n\r\n") >= 0) break;
      delay(5);
    }
    client.stop();
  } else {
    WiFiClient client;
    if (!client.connect(ip, port, timeoutMs)) return "";
    client.print("GET / HTTP/1.1\r\nHost: " + ip.toString() + "\r\nConnection: close\r\n\r\n");
    unsigned long start = millis();
    while ((client.connected() || client.available()) && millis() - start < (unsigned long)timeoutMs) {
      while (client.available()) response += (char)client.read();
      if (response.indexOf("\r\n\r\n") >= 0) break;
      delay(5);
    }
    client.stop();
  }
  int idx = response.indexOf("Server:");
  if (idx < 0) idx = response.indexOf("server:");
  if (idx < 0) return "";
  int lineEnd = response.indexOf('\r', idx);
  if (lineEnd < 0) lineEnd = response.indexOf('\n', idx);
  if (lineEnd < 0) lineEnd = response.length();
  return response.substring(idx, lineEnd);
}

void portScanTask(void *param) {
  String target = portScanTargetInput;
  IPAddress ip;
  bool resolved = ip.fromString(target);
  if (!resolved) {
    resolved = WiFi.hostByName(target.c_str(), ip) == 1;
  }
  if (!resolved) {
    portScanResult = "Could not resolve target: " + target;
    portScanRunning = false;
    vTaskDelete(NULL);
    return;
  }

  String result = "Target: " + target + "  (" + ip.toString() + ")\n";

  int ttl = -1;
  bool sawTtl = icmpPingForTtl(ip, 1500, &ttl);
  result += "\n";

  if (portScanSingleMode) {
    result += "Scanning single port " + String(portScanSinglePort) + "/" + (portScanSingleIsUdp ? "udp" : "tcp") + "...\n";
    String singleBanners = "";
    if (portScanSingleIsUdp) {
      int icmpSock = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
      struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 50000;
      if (icmpSock >= 0) setsockopt(icmpSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      String state = (icmpSock >= 0) ? udpPortState(ip, portScanSinglePort, icmpSock, 1000) : "unknown (socket error)";
      if (icmpSock >= 0) close(icmpSock);
      result += String(portScanSinglePort) + "/udp: " + state + "\n";
    } else {
      bool open = tcpPortOpen(ip, portScanSinglePort, 1000);
      result += String(portScanSinglePort) + "/tcp: " + (open ? "OPEN" : "closed") + "\n";
      if (open) {
        if (portScanSinglePort == 22) {
          String b = grabSshBanner(ip, 1000);
          if (b.length() > 0) { result += "SSH banner: " + b + "\n"; singleBanners += b + " "; }
        } else if (portScanSinglePort == 80) {
          String b = grabHttpServerHeader(ip, 80, false, 1000);
          if (b.length() > 0) { result += "HTTP banner: " + b + "\n"; singleBanners += b + " "; }
        } else if (portScanSinglePort == 443) {
          String b = grabHttpServerHeader(ip, 443, true, 2000);
          if (b.length() > 0) { result += "HTTPS banner: " + b + "\n"; singleBanners += b + " "; }
        }
      }
    }
    result += "\nOS guess: " + guessOsFromSignals(ttl, sawTtl, false, false, portScanSinglePort == 22, singleBanners) + "\n";
  } else {
    result += "TCP ports:\n";
    bool rdpOpen = false, smbOpen = false, sshOpen = false, httpOpen = false, httpsOpen = false;
    for (int i = 0; i < TCP_COMMON_PORTS_COUNT; i++) {
      bool open = tcpPortOpen(ip, TCP_COMMON_PORTS[i].port, 400);
      if (open) {
        result += "  " + String(TCP_COMMON_PORTS[i].port) + "/tcp  " + TCP_COMMON_PORTS[i].name + "  OPEN\n";
        if (TCP_COMMON_PORTS[i].port == 3389) rdpOpen = true;
        if (TCP_COMMON_PORTS[i].port == 445) smbOpen = true;
        if (TCP_COMMON_PORTS[i].port == 22) sshOpen = true;
        if (TCP_COMMON_PORTS[i].port == 80) httpOpen = true;
        if (TCP_COMMON_PORTS[i].port == 443) httpsOpen = true;
      }
    }

    String banners = "";
    if (sshOpen) {
      String b = grabSshBanner(ip, 1000);
      if (b.length() > 0) { result += "\nSSH banner: " + b + "\n"; banners += b + " "; }
    }
    if (httpOpen) {
      String b = grabHttpServerHeader(ip, 80, false, 1000);
      if (b.length() > 0) { result += "HTTP banner: " + b + "\n"; banners += b + " "; }
    }
    if (httpsOpen) {
      String b = grabHttpServerHeader(ip, 443, true, 2000);
      if (b.length() > 0) { result += "HTTPS banner: " + b + "\n"; banners += b + " "; }
    }

    result += "\nUDP ports (no response is shown as open|filtered - UDP can't reliably tell open vs filtered without a protocol-specific probe):\n";
    int icmpSock = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
    struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 50000;
    if (icmpSock >= 0) setsockopt(icmpSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (int i = 0; i < UDP_COMMON_PORTS_COUNT; i++) {
      String state = (icmpSock >= 0) ? udpPortState(ip, UDP_COMMON_PORTS[i].port, icmpSock, 600) : "unknown (socket error)";
      if (state != "closed") {
        result += "  " + String(UDP_COMMON_PORTS[i].port) + "/udp  " + UDP_COMMON_PORTS[i].name + "  " + state + "\n";
      }
    }
    if (icmpSock >= 0) close(icmpSock);

    result += "\nOS guess: " + guessOsFromSignals(ttl, sawTtl, rdpOpen, smbOpen, sshOpen, banners) + "\n";
  }

  portScanResult = result;
  portScanRunning = false;
  vTaskDelete(NULL);
}
// ===== END PORT SCAN TOOL =====

// ===== SUBNET SWEEP TOOL (ARP discovery + port/protocol sweep) =====
#include <lwip/netif.h>
#include <lwip/ip_addr.h>
#include <lwip/etharp.h>
#include <lwip/tcpip.h>
#include <esp_netif_net_stack.h>
#include <esp_interface.h>

extern esp_netif_t *get_esp_interface_netif(esp_interface_t interface);

#define SUBNET_SWEEP_MAX_HOSTS 512

bool arpSweepRunning = false;
String arpSweepResult = "";
uint32_t arpAliveHosts[SUBNET_SWEEP_MAX_HOSTS];
uint8_t arpAliveMacs[SUBNET_SWEEP_MAX_HOSTS][6];
int arpAliveCount = 0;

bool portSweepRunning = false;
String portSweepResult = "";
int portSweepPort = 0;
bool portSweepIsUdp = false;

uint32_t ipToUint32(IPAddress ip) {
  return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) | ((uint32_t)ip[2] << 8) | (uint32_t)ip[3];
}
IPAddress uint32ToIp(uint32_t v) {
  return IPAddress((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

struct netif *getStaNetif() {
  esp_netif_t *espNetif = get_esp_interface_netif(ESP_IF_WIFI_STA);
  if (!espNetif) return nullptr;
  return (struct netif *)esp_netif_get_netif_impl(espNetif);
}

void arpSweepTask(void *param) {
  struct netif *nif = getStaNetif();
  if (!nif) {
    arpSweepResult = "Could not get the Wi-Fi network interface - is the device connected to a network?";
    arpSweepRunning = false;
    vTaskDelete(NULL);
    return;
  }

  uint32_t ipN = ipToUint32(WiFi.localIP());
  uint32_t maskN = ipToUint32(WiFi.subnetMask());
  uint32_t network = ipN & maskN;
  uint32_t broadcast = network | (~maskN);

  int totalHosts = (int)(broadcast - network - 1);
  bool truncated = false;
  int scanCount = totalHosts;
  if (scanCount > SUBNET_SWEEP_MAX_HOSTS) {
    scanCount = SUBNET_SWEEP_MAX_HOSTS;
    truncated = true;
  }

  uint32_t *candidates = (uint32_t *)malloc(sizeof(uint32_t) * scanCount);
  int candidateCount = 0;
  for (uint32_t h = network + 1; h < broadcast && candidateCount < scanCount; h++) {
    if (h == ipN) continue;
    candidates[candidateCount++] = h;
  }

  for (int i = 0; i < candidateCount; i++) {
    ip4_addr_t target;
    IP4_ADDR(&target, (candidates[i] >> 24) & 0xFF, (candidates[i] >> 16) & 0xFF, (candidates[i] >> 8) & 0xFF, candidates[i] & 0xFF);
    LOCK_TCPIP_CORE();
    etharp_request(nif, &target);
    UNLOCK_TCPIP_CORE();
    delay(5);
  }

  delay(1000);

  arpAliveCount = 0;
  for (int i = 0; i < candidateCount && arpAliveCount < SUBNET_SWEEP_MAX_HOSTS; i++) {
    ip4_addr_t target;
    IP4_ADDR(&target, (candidates[i] >> 24) & 0xFF, (candidates[i] >> 16) & 0xFF, (candidates[i] >> 8) & 0xFF, candidates[i] & 0xFF);
    struct eth_addr *ethRet = nullptr;
    const ip4_addr_t *ipRet = nullptr;
    LOCK_TCPIP_CORE();
    s8_t found = etharp_find_addr(nif, &target, &ethRet, &ipRet);
    UNLOCK_TCPIP_CORE();
    if (found >= 0 && ethRet != nullptr) {
      arpAliveHosts[arpAliveCount] = candidates[i];
      memcpy(arpAliveMacs[arpAliveCount], ethRet->addr, 6);
      arpAliveCount++;
    }
  }
  free(candidates);

  String result = "Scanned " + String(candidateCount) + " addresses on " + uint32ToIp(network).toString() + "/" + String(32 - __builtin_popcount(~maskN & 0xFFFFFFFF)) + "\n";
  if (truncated) {
    result += "(Subnet has " + String(totalHosts) + " possible hosts - capped the sweep at " + String(SUBNET_SWEEP_MAX_HOSTS) + ")\n";
  }
  result += "\nAlive hosts (" + String(arpAliveCount) + "):\n";
  for (int i = 0; i < arpAliveCount; i++) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             arpAliveMacs[i][0], arpAliveMacs[i][1], arpAliveMacs[i][2],
             arpAliveMacs[i][3], arpAliveMacs[i][4], arpAliveMacs[i][5]);
    result += "  " + uint32ToIp(arpAliveHosts[i]).toString() + "  " + String(macStr) + "\n";
  }
  if (arpAliveCount == 0) {
    result += "  (none found - try again, some devices are slow to answer ARP)\n";
  }

  arpSweepResult = result;
  arpSweepRunning = false;
  vTaskDelete(NULL);
}

void portSweepTask(void *param) {
  if (arpAliveCount == 0) {
    portSweepResult = "No alive hosts cached - run a Subnet Sweep first.";
    portSweepRunning = false;
    vTaskDelete(NULL);
    return;
  }

  String result = "Scanning " + String(arpAliveCount) + " alive host(s) for port " + String(portSweepPort) + "/" + (portSweepIsUdp ? "udp" : "tcp") + "...\n\n";

  int icmpSock = -1;
  if (portSweepIsUdp) {
    icmpSock = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
    struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 50000;
    if (icmpSock >= 0) setsockopt(icmpSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  int openCount = 0;
  for (int i = 0; i < arpAliveCount; i++) {
    IPAddress ip = uint32ToIp(arpAliveHosts[i]);
    bool isOpen = false;
    String state;
    if (portSweepIsUdp) {
      state = (icmpSock >= 0) ? udpPortState(ip, portSweepPort, icmpSock, 500) : "unknown (socket error)";
      isOpen = (state != "closed");
    } else {
      isOpen = tcpPortOpen(ip, portSweepPort, 400);
      state = isOpen ? "OPEN" : "closed";
    }
    if (isOpen) {
      result += "  " + ip.toString() + "  " + state + "\n";
      openCount++;
    }
  }
  if (icmpSock >= 0) close(icmpSock);

  if (openCount == 0) {
    result += "  (no hosts had that port open)\n";
  }

  portSweepResult = result;
  portSweepRunning = false;
  vTaskDelete(NULL);
}
// ===== END SUBNET SWEEP TOOL =====

// ===== SYSTEM STATUS TOOL =====
#include "driver/temperature_sensor.h"

volatile uint32_t loopCounter = 0;
unsigned long lastLoopRateCalcMs = 0;
float lastLoopHz = 0;

temperature_sensor_handle_t tempSensorHandle = NULL;
bool tempSensorOk = false;

String sysStatusTaskList() {
  String list = "";
  list += "{\"name\":\"WiFi Scan\",\"running\":" + String(wifiScanTaskRunning ? "true" : "false") + "},";
  list += "{\"name\":\"Ping\",\"running\":" + String(pingTaskRunning ? "true" : "false") + "},";
  list += "{\"name\":\"Traceroute\",\"running\":" + String(traceTaskRunning ? "true" : "false") + "},";
  list += "{\"name\":\"DNS Lookup\",\"running\":" + String(dnsLookupRunning ? "true" : "false") + "},";
  list += "{\"name\":\"Reverse DNS\",\"running\":" + String(reverseDnsRunning ? "true" : "false") + "},";
  list += "{\"name\":\"WHOIS\",\"running\":" + String(whoisTaskRunning ? "true" : "false") + "},";
  list += "{\"name\":\"BLE Scan\",\"running\":" + String(bleScanTaskRunning ? "true" : "false") + "},";
  list += "{\"name\":\"BLE Radar\",\"running\":" + String(radarActive ? "true" : "false") + "},";
  extern bool ridScanActive;
  list += "{\"name\":\"Drone RID Scan\",\"running\":" + String(ridScanActive ? "true" : "false") + "},";
  list += "{\"name\":\"Port Scan\",\"running\":" + String(portScanRunning ? "true" : "false") + "},";
  list += "{\"name\":\"Subnet Sweep\",\"running\":" + String(arpSweepRunning ? "true" : "false") + "},";
  list += "{\"name\":\"Port Sweep\",\"running\":" + String(portSweepRunning ? "true" : "false") + "}";
  return list;
}
// ===== END SYSTEM STATUS TOOL =====

// ===== TFTP STORAGE / UPLOAD =====
#include <SPIFFS.h>

#define TFTP_FREE_MARGIN (8UL * 1024UL)
File tftpUploadFile;
String tftpUploadError = "";
size_t tftpUploadReceived = 0;
bool tftpUploadTooBig = false;
size_t tftpUploadCap = 0;

String tftpSanitizeFilename(String filename) {
  int slashIdx = filename.lastIndexOf('/');
  if (slashIdx >= 0) filename = filename.substring(slashIdx + 1);
  if (filename.length() == 0) filename = "upload.bin";
  if (!filename.startsWith("/")) filename = "/" + filename;
  return filename;
}

String tftpJsonEscape(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  return s;
}
// ===== END TFTP STORAGE / UPLOAD =====

// ===== TFTP NETWORK DAEMON =====
#define TFTP_OP_RRQ 1
#define TFTP_OP_WRQ 2
#define TFTP_OP_DATA 3
#define TFTP_OP_ACK 4
#define TFTP_OP_ERROR 5
#define TFTP_BLOCK_SIZE 512
#define TFTP_TIMEOUT_MS 2000
#define TFTP_MAX_RETRIES 5

WiFiUDP tftpUdp;
bool tftpDaemonActive = false;
TaskHandle_t tftpDaemonTaskHandle = NULL;

enum TftpState { TFTP_IDLE, TFTP_SENDING };
TftpState tftpState = TFTP_IDLE;
File tftpDownloadFile;
IPAddress tftpClientIp;
uint16_t tftpClientPort = 0;
uint16_t tftpBlockNum = 0;
unsigned long tftpLastSendMs = 0;
int tftpRetries = 0;
bool tftpTransferDone = false;
uint8_t tftpLastPacket[4 + TFTP_BLOCK_SIZE];
size_t tftpLastPacketLen = 0;

void sendTftpError(IPAddress ip, uint16_t port, uint16_t code, const char* msg) {
  uint8_t packet[132];
  packet[0] = 0;
  packet[1] = TFTP_OP_ERROR;
  packet[2] = (code >> 8) & 0xFF;
  packet[3] = code & 0xFF;
  size_t msgLen = strlen(msg);
  if (msgLen > 120) msgLen = 120;
  memcpy(packet + 4, msg, msgLen);
  packet[4 + msgLen] = 0;
  tftpUdp.beginPacket(ip, port);
  tftpUdp.write(packet, 5 + msgLen);
  tftpUdp.endPacket();
}

void sendTftpDataBlock() {
  uint8_t packet[4 + TFTP_BLOCK_SIZE];
  packet[0] = 0;
  packet[1] = TFTP_OP_DATA;
  packet[2] = (tftpBlockNum >> 8) & 0xFF;
  packet[3] = tftpBlockNum & 0xFF;
  int n = tftpDownloadFile.read(packet + 4, TFTP_BLOCK_SIZE);
  if (n < 0) n = 0;
  size_t packetLen = 4 + n;
  memcpy(tftpLastPacket, packet, packetLen);
  tftpLastPacketLen = packetLen;
  tftpUdp.beginPacket(tftpClientIp, tftpClientPort);
  tftpUdp.write(packet, packetLen);
  tftpUdp.endPacket();
  tftpLastSendMs = millis();
  tftpRetries = 0;
  tftpTransferDone = (n < TFTP_BLOCK_SIZE);
}

void handleTftpRrq(uint8_t* buf, int len, IPAddress fromIp, uint16_t fromPort) {
  String filename;
  int i = 2;
  while (i < len && buf[i] != 0) {
    filename += (char)buf[i];
    i++;
  }
  String path = tftpSanitizeFilename(filename);
  if (tftpState == TFTP_SENDING) {
    sendTftpError(fromIp, fromPort, 0, "Server busy with another transfer");
    return;
  }
  if (!SPIFFS.exists(path)) {
    sendTftpError(fromIp, fromPort, 1, "File not found");
    return;
  }
  tftpDownloadFile = SPIFFS.open(path, "r");
  if (!tftpDownloadFile) {
    sendTftpError(fromIp, fromPort, 0, "Could not open file");
    return;
  }
  tftpClientIp = fromIp;
  tftpClientPort = fromPort;
  tftpBlockNum = 1;
  tftpRetries = 0;
  tftpState = TFTP_SENDING;
  sendTftpDataBlock();
}

void handleTftpAck(uint16_t ackBlock) {
  if (ackBlock != tftpBlockNum) return;
  if (tftpTransferDone) {
    tftpDownloadFile.close();
    tftpState = TFTP_IDLE;
    return;
  }
  tftpBlockNum++;
  sendTftpDataBlock();
}

void tftpDaemonTask(void* param) {
  tftpUdp.begin(69);
  uint8_t buf[600];
  while (tftpDaemonActive) {
    int packetSize = tftpUdp.parsePacket();
    if (packetSize > 0) {
      int len = tftpUdp.read(buf, sizeof(buf));
      if (len >= 2) {
        uint16_t opcode = (buf[0] << 8) | buf[1];
        IPAddress fromIp = tftpUdp.remoteIP();
        uint16_t fromPort = tftpUdp.remotePort();
        if (opcode == TFTP_OP_RRQ) {
          handleTftpRrq(buf, len, fromIp, fromPort);
        } else if (opcode == TFTP_OP_ACK && tftpState == TFTP_SENDING && fromIp == tftpClientIp && fromPort == tftpClientPort && len >= 4) {
          uint16_t ackBlock = (buf[2] << 8) | buf[3];
          handleTftpAck(ackBlock);
        }
      }
    } else if (tftpState == TFTP_SENDING && millis() - tftpLastSendMs > TFTP_TIMEOUT_MS) {
      tftpRetries++;
      if (tftpRetries > TFTP_MAX_RETRIES) {
        tftpDownloadFile.close();
        tftpState = TFTP_IDLE;
      } else {
        tftpUdp.beginPacket(tftpClientIp, tftpClientPort);
        tftpUdp.write(tftpLastPacket, tftpLastPacketLen);
        tftpUdp.endPacket();
        tftpLastSendMs = millis();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (tftpDownloadFile) tftpDownloadFile.close();
  tftpState = TFTP_IDLE;
  tftpUdp.stop();
  tftpDaemonTaskHandle = NULL;
  vTaskDelete(NULL);
}
// ===== END TFTP NETWORK DAEMON =====

// ===== DRONE REMOTE ID SCANNER (Bluetooth only) =====
#define RID_MAX_DRONES 16
struct RidDrone {
  String mac;
  String serial;
  String registration;
  String operatorId;
  int rssi;
  unsigned long lastSeenMs;

  // Basic ID extra
  int uaType;

  // Location / Vector message
  int opStatus;
  int heightType;
  float track;
  float hSpeed;
  bool vSpeedValid;
  float vSpeed;
  bool locationValid;
  double latitude;
  double longitude;
  bool altValid;
  float altPressure;
  float altGeodetic;
  float height;
  int horizAccuracy;
  int vertAccuracy;
  int baroAccuracy;
  int speedAccuracy;
  float timestamp;
  int timestampAccuracy;

  // System message
  int classificationType;
  int operatorLocationType;
  bool operatorLocationValid;
  double operatorLat;
  double operatorLon;
  bool areaValid;
  int areaCount;
  int areaRadius;
  float areaCeiling;
  float areaFloor;
  int categoryEu;
  int classEu;
  bool operatorAltValid;
  float operatorAltGeo;
};
RidDrone ridDrones[RID_MAX_DRONES];
int ridDroneCount = 0;
bool ridScanActive = false;
int ridScanMode = 0;  // 0 = Bluetooth, 1 = WiFi
TaskHandle_t ridScanTaskHandle = NULL;

int ridFindOrAddDrone(const String &mac) {
  for (int i = 0; i < ridDroneCount; i++) {
    if (ridDrones[i].mac.equalsIgnoreCase(mac)) return i;
  }
  if (ridDroneCount < RID_MAX_DRONES) {
    ridDrones[ridDroneCount].mac = mac;
    ridDrones[ridDroneCount].serial = "";
    ridDrones[ridDroneCount].registration = "";
    ridDrones[ridDroneCount].operatorId = "";
    ridDrones[ridDroneCount].rssi = 0;
    ridDrones[ridDroneCount].lastSeenMs = 0;
    ridDrones[ridDroneCount].uaType = -1;
    ridDrones[ridDroneCount].opStatus = -1;
    ridDrones[ridDroneCount].heightType = -1;
    ridDrones[ridDroneCount].track = -1;
    ridDrones[ridDroneCount].hSpeed = -1;
    ridDrones[ridDroneCount].vSpeedValid = false;
    ridDrones[ridDroneCount].vSpeed = 0;
    ridDrones[ridDroneCount].locationValid = false;
    ridDrones[ridDroneCount].latitude = 0;
    ridDrones[ridDroneCount].longitude = 0;
    ridDrones[ridDroneCount].altValid = false;
    ridDrones[ridDroneCount].altPressure = 0;
    ridDrones[ridDroneCount].altGeodetic = 0;
    ridDrones[ridDroneCount].height = 0;
    ridDrones[ridDroneCount].horizAccuracy = -1;
    ridDrones[ridDroneCount].vertAccuracy = -1;
    ridDrones[ridDroneCount].baroAccuracy = -1;
    ridDrones[ridDroneCount].speedAccuracy = -1;
    ridDrones[ridDroneCount].timestamp = -1;
    ridDrones[ridDroneCount].timestampAccuracy = -1;
    ridDrones[ridDroneCount].classificationType = -1;
    ridDrones[ridDroneCount].operatorLocationType = -1;
    ridDrones[ridDroneCount].operatorLocationValid = false;
    ridDrones[ridDroneCount].operatorLat = 0;
    ridDrones[ridDroneCount].operatorLon = 0;
    ridDrones[ridDroneCount].areaValid = false;
    ridDrones[ridDroneCount].areaCount = -1;
    ridDrones[ridDroneCount].areaRadius = -1;
    ridDrones[ridDroneCount].areaCeiling = 0;
    ridDrones[ridDroneCount].areaFloor = 0;
    ridDrones[ridDroneCount].categoryEu = -1;
    ridDrones[ridDroneCount].classEu = -1;
    ridDrones[ridDroneCount].operatorAltValid = false;
    ridDrones[ridDroneCount].operatorAltGeo = 0;
    ridDroneCount++;
    return ridDroneCount - 1;
  }
  return -1;
}

static int32_t ridReadI32LE(const String &d, int off) {
  uint32_t v = (uint32_t)(uint8_t)d[off] | ((uint32_t)(uint8_t)d[off + 1] << 8) |
               ((uint32_t)(uint8_t)d[off + 2] << 16) | ((uint32_t)(uint8_t)d[off + 3] << 24);
  return (int32_t)v;
}
static uint16_t ridReadU16LE(const String &d, int off) {
  return (uint16_t)((uint8_t)d[off] | ((uint16_t)(uint8_t)d[off + 1] << 8));
}

bool ridParseMessagePack(const String &mac, int rssi, const String &packData) {
  int len = packData.length();
  if (len < 2) return false;
  uint8_t packHeader = (uint8_t)packData[0];
  uint8_t packType = (packHeader >> 4) & 0x0F;

  int msgSize = 25;
  int numMessages = 1;
  int msgStart = 0;
  int offset = 0;

  if (packType == 0x0F) {
    offset++;
    if (offset + 1 >= len) return false;
    msgSize = (uint8_t)packData[offset];
    numMessages = (uint8_t)packData[offset + 1];
    offset += 2;
    msgStart = offset;
  }
  if (msgSize <= 0 || msgSize > 64) msgSize = 25;  // sanity fallback

  int droneIdx = ridFindOrAddDrone(mac);
  if (droneIdx < 0) return false;
  ridDrones[droneIdx].rssi = rssi;
  ridDrones[droneIdx].lastSeenMs = millis();

  bool gotSomething = false;
  for (int m = 0; m < numMessages; m++) {
    int base = msgStart + m * msgSize;
    if (base + 2 > len) break;
    uint8_t msgHeader = (uint8_t)packData[base];
    uint8_t msgType = (msgHeader >> 4) & 0x0F;
    if (msgType == 0x0) {  // Basic ID message
      uint8_t idTypeByte = (uint8_t)packData[base + 1];
      uint8_t idType = (idTypeByte >> 4) & 0x0F;
      ridDrones[droneIdx].uaType = (int)(idTypeByte & 0x0F);
      String idStr = "";
      for (int c = 0; c < 20 && (base + 2 + c) < len; c++) {
        char ch = packData[base + 2 + c];
        if (ch == 0) break;
        idStr += ch;
      }
      idStr.trim();
      if (idType == 1 && idStr.length() > 0) {
        ridDrones[droneIdx].serial = idStr;
        gotSomething = true;
      } else if (idType == 2 && idStr.length() > 0) {
        ridDrones[droneIdx].registration = idStr;
        gotSomething = true;
      }
    } else if (msgType == 0x5) {  // Operator ID message
      String opStr = "";
      for (int c = 0; c < 20 && (base + 2 + c) < len; c++) {
        char ch = packData[base + 2 + c];
        if (ch == 0) break;
        opStr += ch;
      }
      opStr.trim();
      if (opStr.length() > 0) {
        ridDrones[droneIdx].operatorId = opStr;
        gotSomething = true;
      }
    } else if (msgType == 0x1 && base + 25 <= len) {  // Location/Vector message
      uint8_t statusByte = (uint8_t)packData[base + 1];
      ridDrones[droneIdx].opStatus = (statusByte >> 4) & 0x0F;
      ridDrones[droneIdx].heightType = (statusByte >> 2) & 0x01;
      uint8_t ewDir = (statusByte >> 1) & 0x01;
      uint8_t speedMult = statusByte & 0x01;

      uint8_t trackRaw = (uint8_t)packData[base + 2];
      ridDrones[droneIdx].track = ewDir ? ((float)trackRaw + 180.0f) : (float)trackRaw;

      uint8_t speedRaw = (uint8_t)packData[base + 3];
      if (speedRaw != 255) {
        ridDrones[droneIdx].hSpeed = speedMult ? ((float)speedRaw * 0.75f + 63.75f) : ((float)speedRaw * 0.25f);
      }

      int8_t vSpeedRaw = (int8_t)packData[base + 4];
      if (vSpeedRaw != 127 && vSpeedRaw != -128) {
        ridDrones[droneIdx].vSpeed = (float)vSpeedRaw * 0.5f;
        ridDrones[droneIdx].vSpeedValid = true;
      }

      int32_t latRaw = ridReadI32LE(packData, base + 5);
      int32_t lonRaw = ridReadI32LE(packData, base + 9);
      if (latRaw != 0 || lonRaw != 0) {
        ridDrones[droneIdx].latitude = (double)latRaw / 1e7;
        ridDrones[droneIdx].longitude = (double)lonRaw / 1e7;
        ridDrones[droneIdx].locationValid = true;
      }

      uint16_t pressAltRaw = ridReadU16LE(packData, base + 13);
      uint16_t geoAltRaw = ridReadU16LE(packData, base + 15);
      uint16_t heightRaw = ridReadU16LE(packData, base + 17);
      if (pressAltRaw != 0xFFFF) ridDrones[droneIdx].altPressure = (float)pressAltRaw * 0.5f - 1000.0f;
      if (geoAltRaw != 0xFFFF) ridDrones[droneIdx].altGeodetic = (float)geoAltRaw * 0.5f - 1000.0f;
      if (heightRaw != 0xFFFF) ridDrones[droneIdx].height = (float)heightRaw * 0.5f - 1000.0f;
      if (pressAltRaw != 0xFFFF || geoAltRaw != 0xFFFF || heightRaw != 0xFFFF) {
        ridDrones[droneIdx].altValid = true;
      }

      uint8_t accByte1 = (uint8_t)packData[base + 19];
      ridDrones[droneIdx].horizAccuracy = accByte1 & 0x0F;
      ridDrones[droneIdx].vertAccuracy = (accByte1 >> 4) & 0x0F;

      uint8_t accByte2 = (uint8_t)packData[base + 20];
      ridDrones[droneIdx].speedAccuracy = accByte2 & 0x0F;
      ridDrones[droneIdx].baroAccuracy = (accByte2 >> 4) & 0x0F;

      uint16_t tsRaw = ridReadU16LE(packData, base + 21);
      if (tsRaw != 0xFFFF) ridDrones[droneIdx].timestamp = (float)tsRaw / 10.0f;

      uint8_t tsAccByte = (uint8_t)packData[base + 23];
      ridDrones[droneIdx].timestampAccuracy = tsAccByte & 0x0F;

      gotSomething = true;
    } else if (msgType == 0x4 && base + 25 <= len) {  // System message
      uint8_t flagsByte = (uint8_t)packData[base + 1];
      ridDrones[droneIdx].classificationType = (flagsByte >> 2) & 0x07;
      ridDrones[droneIdx].operatorLocationType = flagsByte & 0x03;

      int32_t opLatRaw = ridReadI32LE(packData, base + 2);
      int32_t opLonRaw = ridReadI32LE(packData, base + 6);
      if (opLatRaw != 0 || opLonRaw != 0) {
        ridDrones[droneIdx].operatorLat = (double)opLatRaw / 1e7;
        ridDrones[droneIdx].operatorLon = (double)opLonRaw / 1e7;
        ridDrones[droneIdx].operatorLocationValid = true;
      }

      uint16_t areaCountRaw = ridReadU16LE(packData, base + 10);
      uint8_t areaRadiusRaw = (uint8_t)packData[base + 12];
      uint16_t areaCeilRaw = ridReadU16LE(packData, base + 13);
      uint16_t areaFloorRaw = ridReadU16LE(packData, base + 15);
      ridDrones[droneIdx].areaCount = areaCountRaw;
      ridDrones[droneIdx].areaRadius = areaRadiusRaw * 10;
      if (areaCeilRaw != 0xFFFF) ridDrones[droneIdx].areaCeiling = (float)areaCeilRaw * 0.5f - 1000.0f;
      if (areaFloorRaw != 0xFFFF) ridDrones[droneIdx].areaFloor = (float)areaFloorRaw * 0.5f - 1000.0f;
      ridDrones[droneIdx].areaValid = true;

      uint8_t classByte = (uint8_t)packData[base + 17];
      ridDrones[droneIdx].categoryEu = (classByte >> 4) & 0x0F;
      ridDrones[droneIdx].classEu = classByte & 0x0F;

      uint16_t opAltRaw = ridReadU16LE(packData, base + 18);
      if (opAltRaw != 0xFFFF) {
        ridDrones[droneIdx].operatorAltGeo = (float)opAltRaw * 0.5f - 1000.0f;
        ridDrones[droneIdx].operatorAltValid = true;
      }

      gotSomething = true;
    }
  }
  return gotSomething;
}

void ridParseServiceData(const String &mac, int rssi, const String &svcData) {
  int len = svcData.length();
  if (len < 2) return;
  uint8_t appCode = (uint8_t)svcData[0];
  if (appCode != 0x0D) return;  // not ASTM Remote ID application code
  // svcData[1] is an undocumented message-counter byte this device inserts
  // between the App Code and the real pack/message header -- skip it too.
  ridParseMessagePack(mac, rssi, svcData.substring(2));
}

String ridBuildJson() {
  String json = "[";
  unsigned long now = millis();
  for (int i = 0; i < ridDroneCount; i++) {
    if (i > 0) json += ",";
    json += "{\"mac\":\"" + ridDrones[i].mac + "\"";
    json += ",\"serial\":\"" + jsonEscapeForWifiScan(ridDrones[i].serial) + "\"";
    json += ",\"registration\":\"" + jsonEscapeForWifiScan(ridDrones[i].registration) + "\"";
    json += ",\"operatorId\":\"" + jsonEscapeForWifiScan(ridDrones[i].operatorId) + "\"";
    json += ",\"rssi\":" + String(ridDrones[i].rssi);
    json += ",\"agoMs\":" + String(now - ridDrones[i].lastSeenMs);
    json += ",\"uaType\":" + String(ridDrones[i].uaType);
    json += ",\"opStatus\":" + String(ridDrones[i].opStatus);
    json += ",\"heightType\":" + String(ridDrones[i].heightType);
    json += ",\"track\":" + String(ridDrones[i].track, 1);
    json += ",\"hSpeed\":" + String(ridDrones[i].hSpeed, 2);
    json += ",\"vSpeedValid\":" + String(ridDrones[i].vSpeedValid ? "true" : "false");
    json += ",\"vSpeed\":" + String(ridDrones[i].vSpeed, 2);
    json += ",\"locationValid\":" + String(ridDrones[i].locationValid ? "true" : "false");
    json += ",\"lat\":" + String(ridDrones[i].latitude, 7);
    json += ",\"lon\":" + String(ridDrones[i].longitude, 7);
    json += ",\"altValid\":" + String(ridDrones[i].altValid ? "true" : "false");
    json += ",\"altPressure\":" + String(ridDrones[i].altPressure, 1);
    json += ",\"altGeodetic\":" + String(ridDrones[i].altGeodetic, 1);
    json += ",\"height\":" + String(ridDrones[i].height, 1);
    json += ",\"horizAcc\":" + String(ridDrones[i].horizAccuracy);
    json += ",\"vertAcc\":" + String(ridDrones[i].vertAccuracy);
    json += ",\"baroAcc\":" + String(ridDrones[i].baroAccuracy);
    json += ",\"speedAcc\":" + String(ridDrones[i].speedAccuracy);
    json += ",\"timestampAcc\":" + String(ridDrones[i].timestampAccuracy);
    json += ",\"classType\":" + String(ridDrones[i].classificationType);
    json += ",\"opLocType\":" + String(ridDrones[i].operatorLocationType);
    json += ",\"opLocValid\":" + String(ridDrones[i].operatorLocationValid ? "true" : "false");
    json += ",\"opLat\":" + String(ridDrones[i].operatorLat, 7);
    json += ",\"opLon\":" + String(ridDrones[i].operatorLon, 7);
    json += ",\"categoryEu\":" + String(ridDrones[i].categoryEu);
    json += ",\"classEu\":" + String(ridDrones[i].classEu);
    json += "}";
  }
  json += "]";
  return json;
}

String ridBytesToHex(const String &data) {
  String hex = "";
  for (int i = 0; i < data.length(); i++) {
    char buf[4];
    snprintf(buf, sizeof(buf), "%02X ", (uint8_t)data[i]);
    hex += buf;
  }
  return hex;
}

static int ridGapExtDiscHandler(struct ble_gap_event *event, void *arg) {
  if (event->type != BLE_GAP_EVENT_EXT_DISC) {
    return 0;
  }
  const struct ble_gap_ext_disc_desc &d = event->ext_disc;

  char macBuf[18];
  snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
           d.addr.val[5], d.addr.val[4], d.addr.val[3],
           d.addr.val[2], d.addr.val[1], d.addr.val[0]);
  String mac = String(macBuf);

  Serial.print("[RID ext debug] " + mac + " rssi=" + String(d.rssi) + " phy=" + String(d.prim_phy) + "/" + String(d.sec_phy) + " data=[");
  for (int k = 0; k < d.length_data; k++) {
    char hb[4];
    snprintf(hb, sizeof(hb), "%02X ", d.data[k]);
    Serial.print(hb);
  }
  Serial.println("]");

  int i = 0;
  while (i < d.length_data) {
    uint8_t adLen = d.data[i];
    if (adLen == 0) break;
    if (i + 1 + adLen > d.length_data) break;
    uint8_t adType = d.data[i + 1];
    if (adType == 0x16 && adLen >= 3) {
      uint16_t uuid = d.data[i + 2] | (d.data[i + 3] << 8);
      if (uuid == 0xFFFA) {
        String svcData = "";
        for (int k = 0; k < adLen - 3; k++) {
          svcData += (char)d.data[i + 4 + k];
        }
        ridParseServiceData(mac, (int)d.rssi, svcData);
      }
    }
    i += 1 + adLen;
  }
  return 0;
}

#include "esp_wifi.h"

TaskHandle_t wifiRidScanTaskHandle = NULL;

void wifiRidPromiscCallback(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (!ridScanActive) return;
  if (type != WIFI_PKT_MGMT) return;
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  const uint8_t *payload = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  if (len < 24) return;
  uint8_t fcByte0 = payload[0];
  int rssi = pkt->rx_ctrl.rssi;

  char macBuf[18];
  snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
           payload[10], payload[11], payload[12], payload[13], payload[14], payload[15]);
  String mac = String(macBuf);

  if (fcByte0 == 0x80) {
    // Beacon frame: look for the Vendor Specific IE (tag 0xDD) carrying
    // OUI FA:0B:BC, vendor type 0x0D (ASTM/Open Drone ID WiFi Beacon).
    int pos = 36;  // past the 24-byte mgmt header + 12-byte fixed beacon params
    while (pos + 2 <= len) {
      uint8_t tagId = payload[pos];
      uint8_t tagLen = payload[pos + 1];
      if (pos + 2 + tagLen > len) break;
      if (tagId == 0xDD && tagLen >= 4 &&
          payload[pos + 2] == 0xFA && payload[pos + 3] == 0x0B && payload[pos + 4] == 0xBC &&
          payload[pos + 5] == 0x0D) {
        int packDataLen = tagLen - 4;
        const uint8_t *packData = payload + pos + 6;
        // Internal header layout before the message pack isn't confirmed,
        // so try a small window of offsets and accept whichever decodes.
        for (int tryOffset = 0; tryOffset <= 12 && tryOffset < packDataLen; tryOffset++) {
          String chunk = "";
          for (int k = tryOffset; k < packDataLen; k++) chunk += (char)packData[k];
          if (ridParseMessagePack(mac, rssi, chunk)) break;
        }
      }
      pos += 2 + tagLen;
    }
  } else if (fcByte0 == 0xD0) {
    // Public Action frame: WiFi NAN Service Descriptor carrying the ODID
    // message pack. Offsets per the documented NAN action-frame layout.
    if (len < 44) return;
    if (payload[24] != 0x04 || payload[25] != 0x09) return;       // category / action code
    if (payload[26] != 0x50 || payload[27] != 0x6F || payload[28] != 0x9A) return;  // WFA OUI
    if (payload[29] != 0x13) return;                               // OUI type
    if (payload[30] != 0x03) return;                               // Service Descriptor attribute
    static const uint8_t odidServiceId[6] = {0x88, 0x69, 0x19, 0x9D, 0x92, 0x09};
    bool svcMatch = true;
    for (int k = 0; k < 6; k++) {
      if (payload[33 + k] != odidServiceId[k]) { svcMatch = false; break; }
    }
    if (!svcMatch) return;
    int packDataLen = len - 44;
    String chunk = "";
    for (int k = 0; k < packDataLen; k++) chunk += (char)payload[44 + k];
    ridParseMessagePack(mac, rssi, chunk);
  }
}

void wifiRidScanTask(void *param) {
  esp_wifi_set_promiscuous_rx_cb(&wifiRidPromiscCallback);
  esp_wifi_set_promiscuous(true);
  const uint8_t ridWifiChannels[] = {1, 6, 11};
  int ridWifiChIdx = 0;
  while (ridScanActive) {
    esp_wifi_set_channel(ridWifiChannels[ridWifiChIdx], WIFI_SECOND_CHAN_NONE);
    ridWifiChIdx = (ridWifiChIdx + 1) % 3;
    vTaskDelay(pdMS_TO_TICKS(300));
  }
  esp_wifi_set_promiscuous(false);
  wifiRidScanTaskHandle = NULL;
  vTaskDelete(NULL);
}

// ===== BEGIN 802.11 PACKET CAPTURE (PCAP) TOOL =====
bool pcapCaptureActive = false;
bool pcapMgmtOnly = false;
File pcapFile;
unsigned long pcapPacketCount = 0;
unsigned long pcapBytesWritten = 0;
TaskHandle_t pcapCaptureTaskHandle = NULL;
const char *PCAP_FILE_PATH = "/capture.pcap";
const int PCAP_SNAPLEN = 2048;

void pcapWriteGlobalHeader(File &f) {
  uint32_t magic = 0xa1b2c3d4;
  uint16_t vmaj = 2, vmin = 4;
  int32_t zone = 0;
  uint32_t sigfigs = 0;
  uint32_t snaplen = (uint32_t)PCAP_SNAPLEN;
  uint32_t network = 105;  // DLT_IEEE802_11 -- raw 802.11 frames, no radiotap header
  uint8_t hdr[24];
  memcpy(hdr + 0, &magic, 4);
  memcpy(hdr + 4, &vmaj, 2);
  memcpy(hdr + 6, &vmin, 2);
  memcpy(hdr + 8, &zone, 4);
  memcpy(hdr + 12, &sigfigs, 4);
  memcpy(hdr + 16, &snaplen, 4);
  memcpy(hdr + 20, &network, 4);
  f.write(hdr, 24);
}

void pcapPromiscCallback(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (!pcapCaptureActive) return;
  if (pcapMgmtOnly && type != WIFI_PKT_MGMT) return;

  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  int len = pkt->rx_ctrl.sig_len;
  if (len <= 0) return;
  int capLen = (len > PCAP_SNAPLEN) ? PCAP_SNAPLEN : len;

  // Guard against filling the SPIFFS partition -- same free-space check
  // pattern already used by the TFTP file-upload feature.
  size_t freeNow = SPIFFS.totalBytes() - SPIFFS.usedBytes();
  if (freeNow < (size_t)(capLen + 64)) {
    pcapCaptureActive = false;
    return;
  }

  uint32_t nowMs = millis();
  uint32_t ts_sec = nowMs / 1000;
  uint32_t ts_usec = (nowMs % 1000) * 1000;
  uint32_t incl = (uint32_t)capLen;
  uint32_t orig = (uint32_t)len;
  uint8_t rec[16];
  memcpy(rec + 0, &ts_sec, 4);
  memcpy(rec + 4, &ts_usec, 4);
  memcpy(rec + 8, &incl, 4);
  memcpy(rec + 12, &orig, 4);

  pcapFile.write(rec, 16);
  pcapFile.write(pkt->payload, capLen);
  pcapPacketCount++;
  pcapBytesWritten += 16 + capLen;
}

void pcapCaptureTask(void *param) {
  esp_wifi_set_promiscuous_rx_cb(&pcapPromiscCallback);
  esp_wifi_set_promiscuous(true);
  const uint8_t pcapChannels[] = {1, 6, 11};
  int pcapChIdx = 0;
  while (pcapCaptureActive) {
    esp_wifi_set_channel(pcapChannels[pcapChIdx], WIFI_SECOND_CHAN_NONE);
    pcapChIdx = (pcapChIdx + 1) % 3;
    vTaskDelay(pdMS_TO_TICKS(300));
  }
  esp_wifi_set_promiscuous(false);
  pcapFile.close();
  Serial.println("Packet capture stopped. " + String(pcapPacketCount) + " packets, " +
                  String(pcapBytesWritten) + " bytes written to " + String(PCAP_FILE_PATH));
  pcapCaptureTaskHandle = NULL;
  vTaskDelete(NULL);
}
const int PCAP_UPLOAD_PORT = 8080;  // matches the portal's plain-HTTP device-upload listener (see run.sh)

void pcapUploadTask(void *param) {
  if (!meshModeEnabled || !meshServerConnected) {
    Serial.println("Mesh: pcap upload requested but mesh is not connected");
    vTaskDelete(NULL);
    return;
  }
  if (!SPIFFS.exists(PCAP_FILE_PATH)) {
    Serial.println("Mesh: pcap upload requested but no capture file exists");
    vTaskDelete(NULL);
    return;
  }
  File f = SPIFFS.open(PCAP_FILE_PATH, "r");
  if (!f) {
    Serial.println("Mesh: failed to open capture file for upload");
    vTaskDelete(NULL);
    return;
  }
  size_t fileSize = f.size();

  WiFiClient client;  // plain HTTP -- see PCAP_UPLOAD_PORT comment above for why
  HTTPClient http;
  String url = "http://" + meshServerAddr + ":" + String(PCAP_UPLOAD_PORT) +
               "/nodes/" + meshNodeId + "/pcap_upload";
  Serial.println("Mesh: uploading pcap (" + String(fileSize) + " bytes) to " + url);
  if (!http.begin(client, url)) {
    Serial.println("Mesh: pcap upload http.begin() failed");
    f.close();
    vTaskDelete(NULL);
    return;
  }
  http.addHeader("Content-Type", "application/vnd.tcpdump.pcap");
  int code = http.sendRequest("POST", &f, fileSize);
  Serial.println("Mesh: pcap upload result code=" + String(code));
  http.end();
  f.close();
  vTaskDelete(NULL);
}

bool startPcapCaptureIfIdle(bool mgmtOnly) {
  if (pcapCaptureActive) return false;
  if (ridScanActive) return false;
  pcapMgmtOnly = mgmtOnly;
  if (SPIFFS.exists(PCAP_FILE_PATH)) SPIFFS.remove(PCAP_FILE_PATH);
  pcapFile = SPIFFS.open(PCAP_FILE_PATH, "w");
  if (!pcapFile) return false;
  pcapWriteGlobalHeader(pcapFile);
  pcapPacketCount = 0;
  pcapBytesWritten = 24;
  pcapCaptureActive = true;
  xTaskCreate(pcapCaptureTask, "pcapCaptureTask", 4096, NULL, 1, &pcapCaptureTaskHandle);
  Serial.println(String("Packet capture started (") + (pcapMgmtOnly ? "management frames only" : "all 802.11 traffic") + ")");
  return true;
}

void stopPcapCapture() {
  pcapCaptureActive = false;
}

// ===== END 802.11 PACKET CAPTURE (PCAP) TOOL =====

void ridScanTask(void *param) {
  BLEScan *pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(80);
  pBLEScan->setWindow(60);

  struct ble_gap_ext_disc_params ridUncodedParams = {};
  ridUncodedParams.itvl = 64;
  ridUncodedParams.window = 64;
  ridUncodedParams.passive = 0;
  ridUncodedParams.disable_observer_mode = 0;

  struct ble_gap_ext_disc_params ridCodedParams = ridUncodedParams;

  int ridDiscRc = ble_gap_ext_disc(BLE_OWN_ADDR_PUBLIC, 0, 0, 0,
                                    BLE_HCI_SCAN_FILT_NO_WL, 0,
                                    &ridUncodedParams, &ridCodedParams,
                                    ridGapExtDiscHandler, NULL);
  if (ridDiscRc != 0) {
    Serial.println("[RID debug] ble_gap_ext_disc() failed, rc=" + String(ridDiscRc));
  }

  while (ridScanActive) {
    delay(200);
  }
  ble_gap_disc_cancel();
  ridScanTaskHandle = NULL;
  vTaskDelete(NULL);
}
// ===== END DRONE REMOTE ID SCANNER =====

const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><title>FortiGill ESP32-C5 Net Tools</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root{
  --bg:#050b1a;
  --bg2:#0a1630;
  --accent:#1e90ff;
  --accent2:#3aa8ff;
  --neon:#39ff14;
  --neon-dim:#2bb30f;
  --border:#143058;
}
body{font-family:'Courier New',monospace;background:var(--bg);color:var(--neon);padding:1.5rem;max-width:700px;margin:0 auto;}
h1{font-size:1.5rem;color:var(--neon);text-shadow:0 0 8px rgba(57,255,20,0.6);letter-spacing:0.05em;}
.tabs{display:flex;gap:0.5rem;margin-bottom:1rem;flex-wrap:wrap;}
.tab{padding:0.5rem 1rem;background:var(--bg2);border-radius:6px;cursor:pointer;border:1px solid var(--border);color:var(--accent2);transition:all 0.15s;}
.tab:hover{border-color:var(--accent2);box-shadow:0 0 6px rgba(58,168,255,0.5);}
.tab.active{background:var(--accent);color:#001022;font-weight:bold;box-shadow:0 0 10px rgba(30,144,255,0.7);}
.panel{display:none;}
.panel.active{display:block;}
button{padding:0.5rem 1rem;margin-right:0.5rem;font-size:0.95rem;border-radius:6px;border:1px solid var(--accent2);cursor:pointer;background:var(--accent);color:#001022;font-weight:bold;font-family:inherit;transition:all 0.15s;}
button:hover{background:var(--accent2);box-shadow:0 0 10px rgba(58,168,255,0.7);}
button:disabled{opacity:0.35;cursor:not-allowed;background:var(--bg2);color:var(--neon-dim);box-shadow:none;border-color:var(--border);}
button:disabled:hover{background:var(--bg2);box-shadow:none;}
.radar-wrap{display:flex;flex-direction:column;align-items:center;margin-top:1rem;}
.radar-circle{position:relative;width:220px;height:220px;border-radius:50%;background:radial-gradient(circle, rgba(0,255,100,0.08) 0%, rgba(0,255,100,0.02) 70%, transparent 100%);border:1px solid var(--border);overflow:hidden;}
.radar-sweep{position:absolute;top:50%;left:50%;width:50%;height:2px;background:linear-gradient(90deg, rgba(0,255,100,0.9), transparent);transform-origin:0% 50%;animation:radarsweep 2s linear infinite;}
@keyframes radarsweep{from{transform:rotate(0deg);}to{transform:rotate(360deg);}}
.radar-blip{position:absolute;width:12px;height:12px;border-radius:50%;background:#0f0;box-shadow:0 0 8px 2px rgba(0,255,0,0.8);transform:translate(-50%,-50%);transition:top 0.4s, left 0.4s, opacity 0.4s;opacity:0;}
input{padding:0.5rem;border-radius:6px;border:1px solid var(--border);background:var(--bg2);color:var(--neon);margin-right:0.5rem;font-family:inherit;}
input:focus{outline:none;border-color:var(--accent2);box-shadow:0 0 6px rgba(58,168,255,0.5);}
pre{background:var(--bg2);padding:0.8rem;border-radius:6px;white-space:pre-wrap;word-break:break-word;max-height:350px;overflow-y:auto;border:1px solid var(--border);color:var(--neon-dim);}
label{display:block;margin:0.5rem 0 0.2rem;font-size:0.85rem;color:var(--accent2);}
table{color:var(--neon-dim);font-size:0.85rem;}
th{color:var(--accent2);}
</style></head><body>
<h1>FortiGill ESP32-C5 Net Tools</h1>
<div style="font-size:0.75rem;color:var(--accent2);margin-top:-0.5rem;margin-bottom:0.8rem;">v2.12.1</div>
<div id="netStatusBanner" style="margin-bottom:1rem;padding:0.6rem;border:1px solid var(--border);border-radius:6px;background:var(--bg2);font-size:0.9rem;">Loading network status...</div>
<div class="tabs">
  <div class="tab active" data-panel="sysstatus">System Status</div>
  <div class="tab" data-panel="wifiscan">WiFi Scan</div>
  <div class="tab" data-panel="ping">Ping</div>
  <div class="tab" data-panel="traceroute">Traceroute</div>
  <div class="tab" data-panel="dnswhois">DNS / WHOIS</div>
  <div class="tab" data-panel="blescan">BLE Scan</div>
  <div class="tab" data-panel="portscan">Port Scan</div>
  <div class="tab" data-panel="subnetsweep">Subnet Sweep</div>
  <div class="tab" data-panel="wifi">Wi-Fi Settings</div>
  <div class="tab" data-panel="netsettings">Network Settings</div>
  <div class="tab" data-panel="ledctl">LED</div>
  <div class="tab" data-panel="tftpserver">TFTP Server</div>
  <div class="tab" data-panel="dronerid">Drone RID</div>
</div>

<div class="panel active" id="sysstatus">
  <p>A snapshot of the device's own health - CPU/uptime, memory, flash, network, chip temperature, and which background tools are currently running.</p>
  <p style="font-size:0.85rem;color:var(--neon-dim);">Updates live while this tab is open.</p>
  <div style="display:flex;flex-wrap:wrap;gap:0.8rem;margin-top:1rem;">
    <div style="flex:1;min-width:220px;background:var(--bg2);border:1px solid var(--border);border-radius:6px;padding:0.8rem;">
      <strong>CPU / Uptime</strong>
      <div id="sysCpuCard" style="margin-top:0.4rem;font-size:0.9rem;">Not monitoring.</div>
    </div>
    <div style="flex:1;min-width:220px;background:var(--bg2);border:1px solid var(--border);border-radius:6px;padding:0.8rem;">
      <strong>Memory</strong>
      <div id="sysMemCard" style="margin-top:0.4rem;font-size:0.9rem;">Not monitoring.</div>
    </div>
    <div style="flex:1;min-width:220px;background:var(--bg2);border:1px solid var(--border);border-radius:6px;padding:0.8rem;">
      <strong>Flash / Storage</strong>
      <div id="sysFlashCard" style="margin-top:0.4rem;font-size:0.9rem;">Not monitoring.</div>
    </div>
    <div style="flex:1;min-width:220px;background:var(--bg2);border:1px solid var(--border);border-radius:6px;padding:0.8rem;">
      <strong>Network</strong>
      <div id="sysNetCard" style="margin-top:0.4rem;font-size:0.9rem;">Not monitoring.</div>
    </div>
    <div style="flex:1;min-width:220px;background:var(--bg2);border:1px solid var(--border);border-radius:6px;padding:0.8rem;">
      <strong>Chip Temperature</strong>
      <div id="sysTempCard" style="margin-top:0.4rem;font-size:0.9rem;">Not monitoring.</div>
    </div>
    <div style="flex:1;min-width:220px;background:var(--bg2);border:1px solid var(--border);border-radius:6px;padding:0.8rem;">
      <strong>Active Tools</strong>
      <div id="sysTasksCard" style="margin-top:0.4rem;font-size:0.9rem;">Not monitoring.</div>
    </div>
  </div>
</div>

<div class="panel" id="wifiscan">
  <p>This chip has a real dual-band Wi-Fi 6 radio. Shows channel, security, which 802.11 standards each AP advertises, whether it's running Wi-Fi 6 (802.11ax), channel width, and WPS/FTM flags. Only the band you're currently connected on is scannable - switching bands while connected has reliably frozen the device, so the other button disables itself automatically.</p>
  <button id="scan24Btn" onclick="runWifiScan('2g')">Scan 2.4GHz</button>
  <button id="scan5Btn" onclick="runWifiScan('5g')">Scan 5GHz</button>
  <pre id="wifiScanResult" style="margin-top:1rem;">Choose a band and press Scan.</pre>
</div>

<div class="panel" id="ping">
  <p>Pings a target 5 times and reports round-trip statistics, or continuously until stopped.</p>
  <label>Target (IP address or hostname)</label>
  <input id="pingTarget" placeholder="e.g. 8.8.8.8 or example.com">
  <label style="display:flex;align-items:center;gap:0.4rem;margin-top:0.4rem;">
    <input type="checkbox" id="pingAdvToggle" style="width:auto;margin:0;" onchange="togglePingAdvanced()"> Customize TTL / packet size
  </label>
  <div id="pingAdvFields" style="display:none;margin-top:0.4rem;">
    <label>TTL (1-255)</label>
    <input id="pingTtl" type="number" min="1" max="255" value="64" style="width:100px;">
    <label>Packet size in bytes (0-1400)</label>
    <input id="pingSize" type="number" min="0" max="1400" value="64" style="width:100px;">
  </div>
  <label style="display:flex;align-items:center;gap:0.4rem;margin-top:0.4rem;">
    <input type="checkbox" id="pingContinuousToggle" style="width:auto;margin:0;"> Continuous (ping until stopped)
  </label>
  <div style="margin-top:0.8rem;">
    <button id="pingStartBtn" onclick="runPing()">Ping</button>
    <button id="pingStopBtn" onclick="stopPing()" style="display:none;margin-left:0.6rem;">Stop Ping</button>
  </div>
  <canvas id="pingChart" width="600" height="140" style="width:100%;height:140px;margin-top:0.8rem;border:1px solid var(--neon-dim);display:none;"></canvas>
  <div id="pingStats" style="margin-top:0.8rem;font-size:0.9rem;display:none;"></div>
  <pre id="pingResult" style="margin-top:1rem;max-height:300px;overflow-y:auto;">Enter a target and press Ping.</pre>
</div>

<div class="panel" id="traceroute">
  <p>Traces the network path to a target, hop by hop.</p>
  <label>Target (IP address or hostname)</label>
  <input id="traceTarget" placeholder="e.g. 8.8.8.8 or example.com">
  <label style="display:flex;align-items:center;gap:0.4rem;margin-top:0.4rem;">
    <input type="checkbox" id="traceAdvToggle" style="width:auto;margin:0;" onchange="toggleTraceAdvanced()"> Customize max hops / timeout
  </label>
  <div id="traceAdvFields" style="display:none;margin-top:0.4rem;">
    <label>Max hops (1-30)</label>
    <input id="traceMaxHops" type="number" min="1" max="30" value="30" style="width:100px;">
    <label>Timeout per hop in ms (200-5000)</label>
    <input id="traceTimeout" type="number" min="200" max="5000" value="2000" style="width:100px;">
  </div>
  <div style="margin-top:0.8rem;">
    <button onclick="runTraceroute()">Traceroute</button>
  </div>
  <pre id="traceResult" style="margin-top:1rem;">Enter a target and press Traceroute.</pre>
</div>

<div class="panel" id="dnswhois">
  <p>Looks up DNS records for a hostname (A, CNAME, NS, MX, TXT) or does a reverse PTR lookup for an IP, and runs a WHOIS query (via IANA referral to the real registry).</p>
  <label>Target (IP address or hostname)</label>
  <input id="dnsWhoisTarget" placeholder="e.g. example.com or 8.8.8.8">
  <div style="margin-top:0.8rem;">
    <button onclick="runDnsLookup()">DNS Lookup</button>
    <button onclick="runReverseDns()" style="margin-left:0.6rem;">Reverse DNS</button>
    <button onclick="runWhois()" style="margin-left:0.6rem;">WHOIS</button>
  </div>
  <p style="margin-top:1rem;"><strong>DNS results</strong></p>
  <pre id="dnsResult">Enter a target and press DNS Lookup.</pre>
  <p style="margin-top:1rem;"><strong>Reverse DNS results</strong></p>
  <pre id="reverseDnsResultBox">Enter an IP address and press Reverse DNS.</pre>
  <p style="margin-top:1rem;"><strong>WHOIS results</strong></p>
  <pre id="whoisResult">Enter a target and press WHOIS.</pre>
</div>

<div class="panel" id="blescan">
  <div id="bleListView">
    <p>Scans for nearby BLE devices (takes about 8 seconds) and guesses what kind of device each one is.</p>
    <button onclick="runBleScan()">Scan for BLE Devices</button>
    <div id="bleDeviceList" style="margin-top:1rem;"></div>
    <pre id="bleScanResult" style="margin-top:1rem;">Press Scan to look for nearby BLE devices.</pre>
  </div>
  <div id="bleRadarView" style="display:none;">
    <p>Tracking <strong id="radarTargetLabel"></strong> - walk around and watch the signal. This shows relative distance only, not direction.</p>
    <div class="radar-wrap">
      <div class="radar-circle">
        <div class="radar-sweep"></div>
        <div class="radar-blip" id="radarBlip"></div>
      </div>
      <div id="radarReadout" style="margin-top:0.8rem;text-align:center;">Searching...</div>
    </div>
    <button onclick="stopRadar()" style="margin-top:1rem;">Stop &amp; Back to List</button>
  </div>
</div>

<div class="panel" id="portscan">
  <p>Scans a target for common open TCP/UDP ports and tries to guess the operating system from the ICMP TTL and which ports are open.</p>
  <label>Target (IP address or hostname)</label>
  <input id="portScanTarget" placeholder="e.g. 8.8.8.8 or example.com">
  <label style="display:flex;align-items:center;gap:0.4rem;margin-top:0.4rem;">
    <input type="checkbox" id="portScanSingleToggle" style="width:auto;margin:0;" onchange="togglePortScanSingle()"> Scan a specific port instead of the common-ports list
  </label>
  <div id="portScanSingleFields" style="display:none;margin-top:0.4rem;">
    <label>Port</label>
    <input id="portScanSinglePort" type="number" min="1" max="65535" value="80" style="width:100px;">
    <label>Protocol</label>
    <select id="portScanSingleProto" style="padding:0.5rem;border-radius:6px;border:1px solid var(--border);background:var(--bg2);color:var(--neon);font-family:inherit;">
      <option value="tcp">TCP</option>
      <option value="udp">UDP</option>
    </select>
  </div>
  <div style="margin-top:0.8rem;">
    <button onclick="runPortScan()">Scan</button>
  </div>
  <pre id="portScanResult" style="margin-top:1rem;">Enter a target and press Scan.</pre>
</div>

<div class="panel" id="subnetsweep">
  <p>Sends real ARP requests to every address on your current subnet to find which ones are alive (works even for hosts that block ping), then lets you sweep a specific port/protocol across whatever it found.</p>
  <button onclick="runArpSweep()">Sweep Subnet</button>
  <pre id="arpSweepResult" style="margin-top:1rem;">Press Sweep Subnet to find alive hosts.</pre>

  <p style="margin-top:1.2rem;"><strong>Port sweep</strong> (uses the hosts found above)</p>
  <label>Port</label>
  <input id="portSweepPort" type="number" min="1" max="65535" value="80" style="width:100px;">
  <label>Protocol</label>
  <select id="portSweepProto" style="padding:0.5rem;border-radius:6px;border:1px solid var(--border);background:var(--bg2);color:var(--neon);font-family:inherit;">
    <option value="tcp">TCP</option>
    <option value="udp">UDP</option>
  </select>
  <div style="margin-top:0.8rem;">
    <button onclick="runPortSweep()">Sweep Port Across Subnet</button>
  </div>
  <pre id="portSweepResult" style="margin-top:1rem;">Run a subnet sweep first, then sweep a port.</pre>
</div>

<div class="panel" id="wifi">
  <p>Configure the WiFi network this device should connect to.</p>
  <label>Network SSID</label>
  <input id="wifiSsid" placeholder="Enter network SSID">
  <label>Password</label>
  <input id="wifiPass" type="password" placeholder="Enter network password">
  <label>Band</label>
  <label style="display:flex;align-items:center;gap:0.4rem;font-size:0.85rem;color:var(--neon-dim);margin:0.3rem 0;">
    <input type="radio" name="bandPref" id="band5gPref" style="width:auto;margin:0;" checked> Prefer 5GHz if available (falls back to 2.4GHz)
  </label>
  <label style="display:flex;align-items:center;gap:0.4rem;font-size:0.85rem;color:var(--neon-dim);margin:0.3rem 0 0.8rem;">
    <input type="radio" name="bandPref" id="band2gOnlyPref" style="width:auto;margin:0;"> 2.4GHz only (locks the device to 2.4GHz)
  </label>
  <button onclick="saveWifi()">Save & Reboot</button>
  <pre id="wifiResult" style="margin-top:1rem;">Settings will save to flash and reboot the device.</pre>

  <hr style="margin:1rem 0;border-color:var(--neon-dim);opacity:0.3;">
  <label style="display:flex;align-items:center;gap:0.4rem;">
    <input type="checkbox" id="meshToggle" style="width:auto;margin:0;" onchange="toggleMeshFields()"> Join a mesh network
  </label>
  <div id="meshFields" style="display:none;margin-top:0.6rem;">
    <label>Node ID <span style="color:var(--neon-dim);font-size:0.8rem;">(used for esp32-c5-&lt;id&gt;.local)</span></label>
    <input id="meshNodeId" placeholder="e.g. backyard1">
    <label>Mesh Name</label>
    <input id="meshNameField" placeholder="e.g. homemesh">
    <label>Mesh Password</label>
    <input id="meshPassField" type="password" placeholder="Shared mesh password">
    <label>Server IP or FQDN</label>
    <input id="meshServerField" placeholder="e.g. 10.13.37.27 or portal.local">
    <button onclick="saveMeshSettings()" style="margin-top:0.6rem;">Save Mesh Settings & Reboot</button>
    <pre id="meshResult" style="margin-top:1rem;">Loading current mesh settings...</pre>
  </div>
</div>

<div class="panel" id="netsettings">
  <p>By default the device uses DHCP. Enable this to assign a fixed IP address instead. Settings save to flash and the device reboots to apply them cleanly.</p>
  <label style="display:flex;align-items:center;gap:0.4rem;">
    <input type="checkbox" id="staticIpToggle" style="width:auto;margin:0;" onchange="toggleStaticIpFields()"> Use a static IP (uncheck for DHCP)
  </label>
  <div id="staticIpFields" style="display:none;margin-top:0.6rem;">
    <label>IP Address</label>
    <input id="staticIpAddr" placeholder="e.g. 192.168.1.50">
    <label>Subnet Mask</label>
    <input id="staticSubnetMask" placeholder="e.g. 255.255.255.0">
    <label>Default Gateway</label>
    <input id="staticGateway" placeholder="e.g. 192.168.1.1">
    <label>DNS Server 1</label>
    <input id="staticDns1" placeholder="e.g. 1.1.1.1">
    <label>DNS Server 2 (optional)</label>
    <input id="staticDns2" placeholder="e.g. 8.8.8.8">
  </div>
  <button onclick="saveNetworkSettings()" style="margin-top:0.8rem;">Save & Reboot</button>
  <pre id="networkResult" style="margin-top:1rem;">Loading current settings...</pre>
</div>

<div class="panel" id="ledctl">
  <p>The onboard RGB LED shows device status automatically: slow blue blink in AP setup mode with no client connected, solid blue once a client connects to the AP, and solid green when connected to your Wi-Fi network.</p>
  <label style="display:flex;align-items:center;gap:0.4rem;">
    <input type="checkbox" id="ledOff" style="width:auto;margin:0;"> Turn LED off (remembered across reboots - only applies if it reconnects to your saved network; AP setup mode always shows LED status)
  </label>
  <pre id="ledResult" style="margin-top:0.6rem;">LED is on.</pre>
</div>

<div class="panel" id="tftpserver">
  <p>Upload files here to store them in flash (limited only by free space left), then download them over the network with a TFTP client once the daemon is started (daemon controls below, once added).</p>
  <div id="tftpStorageInfo" style="margin-bottom:0.8rem;font-size:0.9rem;">Loading storage info...</div>
  <input type="file" id="tftpFileInput">
  <button onclick="uploadTftpFile()" style="margin-left:0.6rem;">Upload</button>
  <pre id="tftpUploadResult" style="margin-top:1rem;">Choose a file and press Upload.</pre>
  <p style="margin-top:1rem;"><strong>Stored files</strong></p>
  <div id="tftpFileList" style="font-size:0.9rem;">Loading...</div>

  <p style="margin-top:1.2rem;"><strong>TFTP Network Daemon</strong></p>
  <p style="font-size:0.85rem;">The TFTP server must be running for clients to retrieve files.</p>
  <div id="tftpDaemonStatus" style="margin-bottom:0.5rem;">Checking status...</div>
  <button onclick="startTftpDaemon()">Start Daemon</button>
  <button onclick="stopTftpDaemon()" style="margin-left:0.6rem;">Stop Daemon</button>
</div>

<div class="panel" id="dronerid">
  <p>Scans for nearby drones broadcasting Remote ID (ASTM F3411). Bluetooth mode covers Legacy and Long Range advertising; WiFi mode covers Beacon and NAN broadcasts (best-effort decoding, hops channels 1/6/11 &mdash; may briefly interrupt this device's own AP while scanning). Shows each drone's MAC address, FAA registration number, and serial number as they're received.</p>
  <div style="margin-bottom:0.5rem;">
    <label><input type="radio" name="ridMode" value="ble" checked> Bluetooth</label>
    <label style="margin-left:1rem;"><input type="radio" name="ridMode" value="wifi"> WiFi (Beacon/NAN)</label>
  </div>
  <div id="ridScanStatus" style="margin-bottom:0.5rem;">Scan stopped.</div>
  <button id="ridStartBtn" onclick="startRidScan()">Start Scan</button>
  <button id="ridStopBtn" onclick="stopRidScan()" style="display:none;margin-left:0.6rem;">Stop Scan</button>
  <div id="ridDroneList" style="margin-top:1rem;font-size:0.9rem;">No drones detected yet.</div>
</div>

<script>
let sysStatusPoll = null;
const startSysStatusMonitoring = function(){
  if (sysStatusPoll) return;
  pollSysStatus();
  sysStatusPoll = setInterval(pollSysStatus, 1500);
};
const stopSysStatusMonitoring = function(){
  if (sysStatusPoll) { clearInterval(sysStatusPoll); sysStatusPoll = null; }
};
const formatUptime = function(sec){
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  return (d > 0 ? d + 'd ' : '') + h + 'h ' + m + 'm ' + s + 's';
};
const pollSysStatus = function(){
  fetch('/sysstatus')
    .then(r=>r.json())
    .then(d=>{
      document.getElementById('sysCpuCard').innerHTML =
        'Uptime: ' + formatUptime(d.uptimeSec) + '<br>' +
        'CPU: ' + d.cpuFreqMHz + ' MHz<br>' +
        'Loop rate: ' + d.loopHz + ' Hz (lower = busier)';
      document.getElementById('sysMemCard').innerHTML =
        'Free: ' + d.heapFree + ' / ' + d.heapTotal + ' bytes<br>' +
        'Min free ever: ' + d.heapMinFree + ' bytes<br>' +
        'Largest block: ' + d.heapLargestBlock + ' bytes';
      document.getElementById('sysFlashCard').innerHTML =
        'Sketch used: ' + d.sketchUsed + ' bytes<br>' +
        'Sketch free: ' + d.sketchFree + ' bytes<br>' +
        'Flash chip: ' + d.flashChipSize + ' bytes';
      document.getElementById('sysNetCard').innerHTML =
        'IP: ' + d.wifiIp + '<br>' +
        'RSSI: ' + d.wifiRssi + ' dBm<br>' +
        'Channel: ' + d.wifiChannel;
      document.getElementById('sysTempCard').innerHTML =
        (d.chipTempC === null) ? 'Unavailable on this build.' : (d.chipTempC + ' &deg;C (die temperature, not ambient)');
      let tasksHtml = '';
      d.tasks.forEach(t => {
        const color = t.running ? 'var(--neon)' : 'var(--neon-dim)';
        tasksHtml += '<div style="color:' + color + ';">' + t.name + ': ' + (t.running ? 'RUNNING' : 'idle') + '</div>';
      });
      document.getElementById('sysTasksCard').innerHTML = tasksHtml;
    })
    .catch(() => {
      document.getElementById('sysCpuCard').textContent = 'Lost contact with the device.';
    });
};

let tftpFreeBytes = 0;
const refreshTftpFiles = function(){
  fetch('/tftpfiles')
    .then(r=>r.json())
    .then(d=>{
      tftpFreeBytes = d.totalBytes - d.usedBytes;
      document.getElementById('tftpStorageInfo').textContent =
        'Storage: ' + d.usedBytes + ' / ' + d.totalBytes + ' bytes used (' + tftpFreeBytes + ' free)';
      const listDiv = document.getElementById('tftpFileList');
      listDiv.innerHTML = '';
      if (d.files.length === 0) {
        listDiv.textContent = '(no files stored)';
      } else {
        d.files.forEach(f => {
          const row = document.createElement('div');
          row.style.marginBottom = '0.3rem';
          row.textContent = f.name + '  (' + f.size + ' bytes)  ';
          const btn = document.createElement('button');
          btn.textContent = 'Delete';
          btn.onclick = () => deleteTftpFile(f.name);
          row.appendChild(btn);
          listDiv.appendChild(row);
        });
      }
    })
    .catch(() => {
      document.getElementById('tftpStorageInfo').textContent = 'Lost contact with the device.';
    });
};
const deleteTftpFile = function(name){
  if (!confirm('Delete ' + name + '?')) return;
  const formData = new URLSearchParams();
  formData.append('name', name);
  fetch('/tftpdelete', { method: 'POST', body: formData })
    .then(r => r.text())
    .then(() => refreshTftpFiles())
    .catch(err => { alert('Delete failed: ' + err.message); });
};
const uploadTftpFile = function(){
  const input = document.getElementById('tftpFileInput');
  if (!input.files || input.files.length === 0) { alert('Choose a file first'); return; }
  const file = input.files[0];
  if (tftpFreeBytes > 0 && file.size > tftpFreeBytes) {
    document.getElementById('tftpUploadResult').textContent = 'Not enough free space - only ' + tftpFreeBytes + ' bytes available.';
    return;
  }
  const formData = new FormData();
  formData.append('file', file, file.name);
  document.getElementById('tftpUploadResult').textContent = 'Uploading...';
  fetch('/tftpupload', { method: 'POST', body: formData })
    .then(r => r.text().then(t => { document.getElementById('tftpUploadResult').textContent = t; refreshTftpFiles(); }))
    .catch(err => {
      document.getElementById('tftpUploadResult').textContent = 'Upload failed: ' + err.message;
    });
};

const pollTftpDaemonStatus = function(){
  fetch('/tftpdaemonstatus')
    .then(r=>r.json())
    .then(d=>{
      document.getElementById('tftpDaemonStatus').textContent = d.running ? 'Daemon: RUNNING (UDP port 69)' : 'Daemon: stopped';
    })
    .catch(() => {
      document.getElementById('tftpDaemonStatus').textContent = 'Lost contact with the device.';
    });
};
const startTftpDaemon = function(){
  fetch('/tftpdaemonstart').then(() => pollTftpDaemonStatus());
};
const stopTftpDaemon = function(){
  fetch('/tftpdaemonstop').then(() => pollTftpDaemonStatus());
};

let ridPoll = null;
const startRidScan = function(){
  const modeEl = document.querySelector('input[name="ridMode"]:checked');
  const mode = modeEl ? modeEl.value : 'ble';
  fetch('/ridscanstart?mode=' + mode).then(() => {
    document.getElementById('ridStartBtn').style.display = 'none';
    document.getElementById('ridStopBtn').style.display = 'inline-block';
    document.getElementById('ridScanStatus').textContent = 'Scanning for Remote ID broadcasts (' + (mode === 'wifi' ? 'WiFi' : 'Bluetooth') + ')...';
    if (ridPoll) clearInterval(ridPoll);
    ridPoll = setInterval(pollRidScan, 1000);
    pollRidScan();
  });
};
const stopRidScan = function(){
  fetch('/ridscanstop').then(() => {
    document.getElementById('ridStartBtn').style.display = 'inline-block';
    document.getElementById('ridStopBtn').style.display = 'none';
    document.getElementById('ridScanStatus').textContent = 'Scan stopped.';
    if (ridPoll) { clearInterval(ridPoll); ridPoll = null; }
  });
};
let ridLastData = null;
const ridExpandedMacs = new Set();
function ridToggleMoreInfo(mac) {
  if (ridExpandedMacs.has(mac)) ridExpandedMacs.delete(mac); else ridExpandedMacs.add(mac);
  if (ridLastData) ridRenderDroneList(ridLastData);
}
const RID_UA_TYPE = ['None','Aeroplane','Helicopter/Multirotor','Gyroplane','Hybrid Lift','Ornithopter','Glider','Kite','Free Balloon','Captive Balloon','Airship','Free Fall/Parachute','Rocket','Tethered Powered Aircraft','Ground Obstacle','Other'];
const RID_OP_STATUS = ['Undeclared','Ground','Airborne','Emergency','Remote ID System Failure'];
const RID_HEIGHT_TYPE = ['Above Takeoff','Above Ground Level'];
const RID_HOR_ACC = ['Unknown','<18.52km (10NM)','<7.408km (4NM)','<3.704km (2NM)','<1852m (1NM)','<926m (0.5NM)','<555.6m (0.3NM)','<185.2m (0.1NM)','<92.6m (0.05NM)','<30m','<10m','<3m','<1m'];
const RID_VER_ACC = ['Unknown/>=150m','<150m','<45m','<25m','<10m','<3m','<1m'];
const RID_SPEED_ACC = ['Unknown/>=10m/s','<10m/s','<3m/s','<1m/s','<0.3m/s'];
const RID_CLASS_TYPE = ['Undeclared','European Union'];
const RID_OP_LOC_TYPE = ['Takeoff Location','Live GNSS','Fixed Location'];
const RID_CATEGORY_EU = ['Undeclared','Open','Specific','Certified'];
const RID_CLASS_EU = ['Undeclared','Class 0','Class 1','Class 2','Class 3','Class 4','Class 5','Class 6'];
function ridLookup(arr, idx) {
  if (idx === undefined || idx === null || idx < 0 || idx >= arr.length) return '(not received yet)';
  return arr[idx];
}
function ridHaversineMeters(lat1, lon1, lat2, lon2) {
  const R = 6371000;
  const dLat = (lat2 - lat1) * Math.PI / 180;
  const dLon = (lon2 - lon1) * Math.PI / 180;
  const a = Math.sin(dLat/2)*Math.sin(dLat/2) + Math.cos(lat1*Math.PI/180)*Math.cos(lat2*Math.PI/180)*Math.sin(dLon/2)*Math.sin(dLon/2);
  return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1-a));
}
function ridRenderDroneList(d) {
  const listDiv = document.getElementById('ridDroneList');
  if (!d.drones || d.drones.length === 0) {
    listDiv.textContent = 'No drones detected yet.';
    return;
  }
  let html = '';
  d.drones.forEach(dr => {
    const agoSec = Math.round(dr.agoMs / 1000);
    const expanded = ridExpandedMacs.has(dr.mac);
    const isEu = dr.classType === 1;
    html += '<div style="margin-bottom:0.6rem;padding:0.4rem;border:1px solid var(--neon-dim);">';
    html += '<b>MAC:</b> ' + dr.mac + '  (RSSI ' + dr.rssi + ' dBm, seen ' + agoSec + 's ago)<br>';
    html += '<b>FAA Registration:</b> ' + (dr.registration || '(not received yet)') + '<br>';
    html += '<b>Serial Number:</b> ' + (dr.serial || '(not received yet)') + '<br>';
    html += '<b>Operator ID:</b> ' + (dr.operatorId || '(not received yet)') + '<br>';
    html += '<button onclick="ridToggleMoreInfo(\'' + dr.mac + '\')" style="margin-top:0.3rem;">' + (expanded ? 'Hide Info &#9650;' : 'More Info &#9660;') + '</button>';
    if (expanded) {
      html += '<div style="margin-top:0.5rem;padding-top:0.4rem;border-top:1px dashed var(--neon-dim);">';
      html += '<u>Aircraft</u><br>';
      html += '<b>Manufacturer:</b> (not part of Remote ID broadcast)<br>';
      html += '<b>Model:</b> (not part of Remote ID broadcast)<br>';
      html += '<b>Type:</b> ' + ridLookup(RID_UA_TYPE, dr.uaType) + '<br>';
      html += '<b>Category (EU):</b> ' + (isEu ? ridLookup(RID_CATEGORY_EU, dr.categoryEu) : 'N/A (not EU-classified)') + '<br>';
      html += '<b>Class (EU):</b> ' + (isEu ? ridLookup(RID_CLASS_EU, dr.classEu) : 'N/A (not EU-classified)') + '<br>';
      html += '<br><u>Operational State</u><br>';
      html += '<b>Status:</b> ' + ridLookup(RID_OP_STATUS, dr.opStatus) + '<br>';
      html += '<b>Location:</b> ' + (dr.locationValid ? (dr.lat.toFixed(7) + ', ' + dr.lon.toFixed(7)) : '(not received yet)') + '<br>';
      html += '<b>Horizontal Accuracy:</b> ' + ridLookup(RID_HOR_ACC, dr.horizAcc) + '<br>';
      html += '<b>Vertical Accuracy:</b> ' + ridLookup(RID_VER_ACC, dr.vertAcc) + '<br>';
      html += '<b>Heading/Track:</b> ' + (dr.track >= 0 ? dr.track.toFixed(0) + '&deg;' : '(not received yet)') + '<br>';
      html += '<b>Distance from Operator:</b> ' + ((dr.locationValid && dr.opLocValid) ? ridHaversineMeters(dr.lat, dr.lon, dr.opLat, dr.opLon).toFixed(0) + ' m' : '(not received yet)') + '<br>';
      html += '<b>Height:</b> ' + (dr.altValid ? (dr.height.toFixed(1) + ' m (' + ridLookup(RID_HEIGHT_TYPE, dr.heightType) + ')') : '(not received yet)') + '<br>';
      html += '<b>Altitude (Geodetic):</b> ' + (dr.altValid ? dr.altGeodetic.toFixed(1) + ' m' : '(not received yet)') + '<br>';
      html += '<b>Altitude (Pressure):</b> ' + (dr.altValid ? dr.altPressure.toFixed(1) + ' m' : '(not received yet)') + '<br>';
      html += '<b>Barometric Accuracy:</b> ' + ridLookup(RID_VER_ACC, dr.baroAcc) + '<br>';
      html += '<br><u>Speed</u><br>';
      html += '<b>Horizontal Speed:</b> ' + (dr.hSpeed >= 0 ? dr.hSpeed.toFixed(2) + ' m/s' : '(not received yet)') + '<br>';
      html += '<b>Vertical Speed:</b> ' + (dr.vSpeedValid ? dr.vSpeed.toFixed(2) + ' m/s' : '(not received yet)') + '<br>';
      html += '<b>Speed Accuracy:</b> ' + ridLookup(RID_SPEED_ACC, dr.speedAcc) + '<br>';
      html += '<b>Time Accuracy:</b> ' + (dr.timestampAcc > 0 ? ('&plusmn;' + (dr.timestampAcc / 10).toFixed(1) + ' s') : '(not received yet)') + '<br>';
      html += '<br><u>Operator</u><br>';
      html += '<b>Operator Location Type:</b> ' + ridLookup(RID_OP_LOC_TYPE, dr.opLocType) + '<br>';
      html += '<b>Operator Location:</b> ' + (dr.opLocValid ? (dr.opLat.toFixed(7) + ', ' + dr.opLon.toFixed(7)) : '(not received yet)') + '<br>';
      html += '</div>';
    }
    html += '</div>';
  });
  listDiv.innerHTML = html;
}
const pollRidScan = function(){
  fetch('/ridscanstatus')
    .then(r=>r.json())
    .then(d=>{
      ridLastData = d;
      ridRenderDroneList(d);
    })
    .catch(() => {
      document.getElementById('ridScanStatus').textContent = 'Lost contact with the device.';
    });
};

document.querySelectorAll('.tab').forEach(t => {
  t.onclick = () => {
    document.querySelectorAll('.tab').forEach(x => x.classList.remove('active'));
    document.querySelectorAll('.panel').forEach(x => x.classList.remove('active'));
    t.classList.add('active');
    document.getElementById(t.dataset.panel).classList.add('active');
    if (t.dataset.panel === 'sysstatus') {
      startSysStatusMonitoring();
    } else {
      stopSysStatusMonitoring();
    }
    if (t.dataset.panel === 'tftpserver') {
      refreshTftpFiles();
      pollTftpDaemonStatus();
    }
  };
});
startSysStatusMonitoring();

document.getElementById('ledOff').onchange = function(){
  const off = this.checked;
  fetch('/led?on=' + (off ? '0' : '1'))
    .then(r=>r.text())
    .then(t=>{ document.getElementById('ledResult').textContent = 'LED is ' + t + ' (remembered).'; });
};
fetch('/led')
  .then(r=>r.text())
  .then(t=>{
    document.getElementById('ledOff').checked = (t === 'off');
    document.getElementById('ledResult').textContent = 'LED is ' + t + '.';
  });

let wifiScanPoll = null;
let wifiScanPollStarted = 0;
let bandPrefLoaded = false;
let wifiScanTicker = null;
const runWifiScan = function(band){
  document.getElementById('wifiScanResult').textContent = 'Starting ' + (band === '5g' ? '5GHz' : '2.4GHz') + ' scan...';
  fetch('/wifiscan?band=' + band).then(() => {
    if (wifiScanPoll) clearInterval(wifiScanPoll);
    if (wifiScanTicker) clearInterval(wifiScanTicker);
    wifiScanPollStarted = Date.now();
    wifiScanPoll = setInterval(pollWifiScan, 300);
    wifiScanTicker = setInterval(() => {
      if (!wifiScanPoll) { clearInterval(wifiScanTicker); wifiScanTicker = null; return; }
      const secs = Math.floor((Date.now() - wifiScanPollStarted) / 1000);
      document.getElementById('wifiScanResult').textContent =
        'Scanning ' + (band === '5g' ? '5GHz' : '2.4GHz') + '... ' + secs + 's elapsed' +
        (band === '5g' ? ' (5GHz has many more channels, so it takes longer - watch the serial monitor for live channel-by-channel progress)' : '');
    }, 1000);
  }).catch(() => {
    document.getElementById('wifiScanResult').textContent = 'Could not reach the device to start the scan.';
  });
};
const pollWifiScan = function(){
  fetch('/wifiscanstatus')
    .then(r=>r.json())
    .then(d=>{
      let text = d.result;
      if (d.running) {
        text += '\n\n(' + Math.floor((Date.now() - wifiScanPollStarted) / 1000) + 's elapsed - 5GHz has many more channels, so it takes longer)';
      }
      document.getElementById('wifiScanResult').textContent = text;
      if (!d.running) {
        clearInterval(wifiScanPoll);
        wifiScanPoll = null;
      }
    })
    .catch(() => {
      if (Date.now() - wifiScanPollStarted > 15000) {
        clearInterval(wifiScanPoll);
        wifiScanPoll = null;
        document.getElementById('wifiScanResult').textContent = 'Lost contact with the device - it may have frozen and need a power cycle.';
      }
    });
};

const togglePingAdvanced = function(){
  const show = document.getElementById('pingAdvToggle').checked;
  document.getElementById('pingAdvFields').style.display = show ? 'block' : 'none';
};
let pingChartData = [];
let pingLastSeenCount = 0;
const pingChartMaxPoints = 60;
const drawPingChart = function(){
  const canvas = document.getElementById('pingChart');
  const ctx = canvas.getContext('2d');
  const w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  if (pingChartData.length < 2) return;

  const leftPad = 42, rightPad = 8, topPad = 8, bottomPad = 18;
  const plotW = w - leftPad - rightPad;
  const plotH = h - topPad - bottomPad;
  const n = pingChartData.length;
  const stepX = plotW / (pingChartMaxPoints - 1);

  const maxVal = Math.max.apply(null, pingChartData);
  const minVal = Math.min.apply(null, pingChartData);
  const range = (maxVal - minVal) || 1;

  const gridColor = getComputedStyle(document.documentElement).getPropertyValue('--neon-dim').trim() || '#666';
  const lineColor = getComputedStyle(document.documentElement).getPropertyValue('--accent2').trim() || '#4da6ff';

  ctx.font = '10px monospace';
  ctx.strokeStyle = gridColor;
  ctx.fillStyle = gridColor;
  ctx.lineWidth = 1;

  // Y axis: gridlines + ms labels
  const yTicks = 4;
  for (let i = 0; i <= yTicks; i++) {
    const frac = i / yTicks;
    const val = maxVal - frac * range;
    const y = topPad + frac * plotH;
    ctx.beginPath();
    ctx.moveTo(leftPad, y);
    ctx.lineTo(leftPad + plotW, y);
    ctx.stroke();
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    ctx.fillText(Math.round(val) + 'ms', leftPad - 4, y);
  }

  // X axis: tick marks + reply-number labels (roughly 1 per second)
  const tickEvery = 10;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  for (let i = 0; i < n; i++) {
    if (i % tickEvery === 0 || i === n - 1) {
      const x = leftPad + i * stepX;
      ctx.beginPath();
      ctx.moveTo(x, topPad + plotH);
      ctx.lineTo(x, topPad + plotH + 4);
      ctx.stroke();
      const replyNum = pingLastSeenCount - (n - 1 - i);
      ctx.fillText('#' + replyNum, x, topPad + plotH + 5);
    }
  }

  // Data line
  ctx.strokeStyle = lineColor;
  ctx.lineWidth = 2;
  ctx.beginPath();
  pingChartData.forEach((v, i) => {
    const x = leftPad + i * stepX;
    const y = topPad + plotH - ((v - minVal) / range) * plotH;
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  });
  ctx.stroke();
};

let pingPoll = null;
let pingPollStarted = 0;
const runPing = function(){
  const target = document.getElementById('pingTarget').value.trim();
  if (!target) { alert('Enter a target IP or hostname'); return; }
  const useAdv = document.getElementById('pingAdvToggle').checked;
  const ttl = useAdv ? document.getElementById('pingTtl').value : 64;
  const size = useAdv ? document.getElementById('pingSize').value : 64;
  const continuous = document.getElementById('pingContinuousToggle').checked;
  document.getElementById('pingResult').textContent = 'Starting ping to ' + target + '...';
  fetch('/ping?target=' + encodeURIComponent(target) + '&ttl=' + ttl + '&size=' + size + '&continuous=' + (continuous ? '1' : '0')).then(r => {
    if (!r.ok) {
      return r.text().then(t => { throw new Error(t); });
    }
    document.getElementById('pingStartBtn').disabled = true;
    document.getElementById('pingStopBtn').style.display = continuous ? 'inline-block' : 'none';
    document.getElementById('pingStats').style.display = 'none';
    pingChartData = [];
    pingLastSeenCount = 0;
    document.getElementById('pingChart').style.display = 'none';
    if (pingPoll) clearInterval(pingPoll);
    pingPollStarted = Date.now();
    pingPoll = setInterval(pollPingStatus, 300);
  }).catch(err => {
    document.getElementById('pingResult').textContent = 'Could not start ping: ' + err.message;
  });
};
const stopPing = function(){
  fetch('/pingstop');
};
const pollPingStatus = function(){
  fetch('/pingstatus')
    .then(r=>r.json())
    .then(d=>{
      document.getElementById('pingResult').textContent = d.result;
      document.getElementById('pingResult').scrollTop = document.getElementById('pingResult').scrollHeight;
      if (d.count > 0) {
        document.getElementById('pingStats').style.display = 'block';
        document.getElementById('pingStats').textContent = 'Replies: ' + d.count + '  |  min/avg/max = ' + d.min + '/' + d.avg + '/' + d.max + ' ms';
        if (d.count > pingLastSeenCount) {
          pingLastSeenCount = d.count;
          pingChartData.push(d.last);
          if (pingChartData.length > pingChartMaxPoints) pingChartData.shift();
          document.getElementById('pingChart').style.display = 'block';
          drawPingChart();
        }
      } else {
        document.getElementById('pingStats').style.display = 'none';
      }
      if (!d.running) {
        clearInterval(pingPoll);
        pingPoll = null;
        document.getElementById('pingStartBtn').disabled = false;
        document.getElementById('pingStopBtn').style.display = 'none';
      }
    })
    .catch(() => {
      if (Date.now() - pingPollStarted > 20000) {
        clearInterval(pingPoll);
        pingPoll = null;
        document.getElementById('pingResult').textContent = 'Lost contact with the device.';
        document.getElementById('pingStartBtn').disabled = false;
        document.getElementById('pingStopBtn').style.display = 'none';
      }
    });
};

const toggleTraceAdvanced = function(){
  const show = document.getElementById('traceAdvToggle').checked;
  document.getElementById('traceAdvFields').style.display = show ? 'block' : 'none';
};
let tracePoll = null;
const runTraceroute = function(){
  const target = document.getElementById('traceTarget').value.trim();
  if (!target) { alert('Enter a target IP or hostname'); return; }
  const useAdv = document.getElementById('traceAdvToggle').checked;
  const maxhops = useAdv ? document.getElementById('traceMaxHops').value : 30;
  const timeout = useAdv ? document.getElementById('traceTimeout').value : 2000;
  document.getElementById('traceResult').textContent = 'Starting traceroute to ' + target + '...';
  fetch('/traceroute?target=' + encodeURIComponent(target) + '&maxhops=' + maxhops + '&timeout=' + timeout).then(r => {
    if (!r.ok) {
      return r.text().then(t => { throw new Error(t); });
    }
    if (tracePoll) clearInterval(tracePoll);
    tracePoll = setInterval(pollTraceroute, 1000);
  }).catch(err => {
    document.getElementById('traceResult').textContent = 'Could not start traceroute: ' + err.message;
  });
};
const pollTraceroute = function(){
  fetch('/traceroutestatus')
    .then(r=>r.json())
    .then(d=>{
      document.getElementById('traceResult').textContent = d.result;
      if (!d.running) {
        clearInterval(tracePoll);
        tracePoll = null;
      }
    })
    .catch(() => {
      clearInterval(tracePoll);
      tracePoll = null;
      document.getElementById('traceResult').textContent = 'Lost contact with the device.';
    });
};

let dnsPoll = null;
const runDnsLookup = function(){
  const target = document.getElementById('dnsWhoisTarget').value.trim();
  if (!target) { alert('Enter a target IP or hostname'); return; }
  document.getElementById('dnsResult').textContent = 'Looking up ' + target + '...';
  fetch('/dnslookup?target=' + encodeURIComponent(target)).then(r => {
    if (!r.ok) return r.text().then(t => { throw new Error(t); });
    if (dnsPoll) clearInterval(dnsPoll);
    dnsPoll = setInterval(pollDnsLookup, 800);
  }).catch(err => {
    document.getElementById('dnsResult').textContent = 'Could not start lookup: ' + err.message;
  });
};
let lastDnsARecord = null;
let lastDnsTarget = null;
const pollDnsLookup = function(){
  fetch('/dnslookupstatus')
    .then(r=>r.json())
    .then(d=>{
      document.getElementById('dnsResult').textContent = d.result;
      if (!d.running) {
        clearInterval(dnsPoll); dnsPoll = null;
        const aMatch = d.result.match(/A records for [^\n]+:\n(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})/);
        if (aMatch) {
          lastDnsARecord = aMatch[1];
          lastDnsTarget = document.getElementById('dnsWhoisTarget').value.trim();
        } else {
          lastDnsARecord = null;
          lastDnsTarget = null;
        }
      }
    })
    .catch(() => {
      clearInterval(dnsPoll); dnsPoll = null;
      document.getElementById('dnsResult').textContent = 'Lost contact with the device.';
    });
};

let reverseDnsPoll = null;
const runReverseDns = function(){
  const typed = document.getElementById('dnsWhoisTarget').value.trim();
  if (!typed) { alert('Enter an IP address'); return; }
  const isIp = /^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(typed);
  let lookupTarget = typed;
  if (!isIp && lastDnsTarget === typed && lastDnsARecord) {
    lookupTarget = lastDnsARecord;
  }
  const note = (lookupTarget !== typed) ? (' (from A record of ' + typed + ')') : '';
  document.getElementById('reverseDnsResultBox').textContent = 'Looking up ' + lookupTarget + note + '...';
  fetch('/reversedns?target=' + encodeURIComponent(lookupTarget)).then(r => {
    if (!r.ok) return r.text().then(t => { throw new Error(t); });
    if (reverseDnsPoll) clearInterval(reverseDnsPoll);
    reverseDnsPoll = setInterval(pollReverseDns, 600);
  }).catch(err => {
    document.getElementById('reverseDnsResultBox').textContent = 'Could not start lookup: ' + err.message;
  });
};
const pollReverseDns = function(){
  fetch('/reversednsstatus')
    .then(r=>r.json())
    .then(d=>{
      document.getElementById('reverseDnsResultBox').textContent = d.result;
      if (!d.running) { clearInterval(reverseDnsPoll); reverseDnsPoll = null; }
    })
    .catch(() => {
      clearInterval(reverseDnsPoll); reverseDnsPoll = null;
      document.getElementById('reverseDnsResultBox').textContent = 'Lost contact with the device.';
    });
};

let whoisPoll = null;
const runWhois = function(){
  const target = document.getElementById('dnsWhoisTarget').value.trim();
  if (!target) { alert('Enter a target IP or hostname'); return; }
  document.getElementById('whoisResult').textContent = 'Running WHOIS for ' + target + '...';
  fetch('/whois?target=' + encodeURIComponent(target)).then(r => {
    if (!r.ok) return r.text().then(t => { throw new Error(t); });
    if (whoisPoll) clearInterval(whoisPoll);
    whoisPoll = setInterval(pollWhois, 800);
  }).catch(err => {
    document.getElementById('whoisResult').textContent = 'Could not start WHOIS: ' + err.message;
  });
};
const pollWhois = function(){
  fetch('/whoisstatus')
    .then(r=>r.json())
    .then(d=>{
      document.getElementById('whoisResult').textContent = d.result;
      if (!d.running) { clearInterval(whoisPoll); whoisPoll = null; }
    })
    .catch(() => {
      clearInterval(whoisPoll); whoisPoll = null;
      document.getElementById('whoisResult').textContent = 'Lost contact with the device.';
    });
};

const togglePortScanSingle = function(){
  const show = document.getElementById('portScanSingleToggle').checked;
  document.getElementById('portScanSingleFields').style.display = show ? 'block' : 'none';
};
let portScanPoll = null;
let portScanPollStarted = 0;
const runPortScan = function(){
  const target = document.getElementById('portScanTarget').value.trim();
  if (!target) { alert('Enter a target IP or hostname'); return; }
  const single = document.getElementById('portScanSingleToggle').checked;
  let url = '/portscan?target=' + encodeURIComponent(target);
  if (single) {
    const port = document.getElementById('portScanSinglePort').value;
    const proto = document.getElementById('portScanSingleProto').value;
    url += '&mode=single&port=' + port + '&proto=' + proto;
  }
  document.getElementById('portScanResult').textContent = 'Starting scan...';
  fetch(url).then(r => {
    if (!r.ok) return r.text().then(t => { throw new Error(t); });
    if (portScanPoll) clearInterval(portScanPoll);
    portScanPollStarted = Date.now();
    portScanPoll = setInterval(pollPortScan, 1000);
  }).catch(err => {
    document.getElementById('portScanResult').textContent = 'Could not start scan: ' + err.message;
  });
};
const pollPortScan = function(){
  fetch('/portscanstatus')
    .then(r=>r.json())
    .then(d=>{
      let text = d.result;
      if (d.running) {
        text += '\n\n(' + Math.floor((Date.now() - portScanPollStarted) / 1000) + 's elapsed - scanning the common-ports list takes 10-20s)';
      }
      document.getElementById('portScanResult').textContent = text;
      if (!d.running) { clearInterval(portScanPoll); portScanPoll = null; }
    })
    .catch(() => {
      clearInterval(portScanPoll); portScanPoll = null;
      document.getElementById('portScanResult').textContent = 'Lost contact with the device.';
    });
};

let arpSweepPoll = null;
let arpSweepPollStarted = 0;
const runArpSweep = function(){
  document.getElementById('arpSweepResult').textContent = 'Starting subnet sweep...';
  fetch('/arpsweep').then(r => {
    if (!r.ok) return r.text().then(t => { throw new Error(t); });
    if (arpSweepPoll) clearInterval(arpSweepPoll);
    arpSweepPollStarted = Date.now();
    arpSweepPoll = setInterval(pollArpSweep, 1000);
  }).catch(err => {
    document.getElementById('arpSweepResult').textContent = 'Could not start sweep: ' + err.message;
  });
};
const pollArpSweep = function(){
  fetch('/arpsweepstatus')
    .then(r=>r.json())
    .then(d=>{
      let text = d.result;
      if (d.running) {
        text += '\n\n(' + Math.floor((Date.now() - arpSweepPollStarted) / 1000) + 's elapsed)';
      }
      document.getElementById('arpSweepResult').textContent = text;
      if (!d.running) { clearInterval(arpSweepPoll); arpSweepPoll = null; }
    })
    .catch(() => {
      clearInterval(arpSweepPoll); arpSweepPoll = null;
      document.getElementById('arpSweepResult').textContent = 'Lost contact with the device.';
    });
};

let portSweepPoll = null;
let portSweepPollStarted = 0;
const runPortSweep = function(){
  const port = document.getElementById('portSweepPort').value;
  const proto = document.getElementById('portSweepProto').value;
  document.getElementById('portSweepResult').textContent = 'Starting port sweep...';
  fetch('/portsweep?port=' + port + '&proto=' + proto).then(r => {
    if (!r.ok) return r.text().then(t => { throw new Error(t); });
    if (portSweepPoll) clearInterval(portSweepPoll);
    portSweepPollStarted = Date.now();
    portSweepPoll = setInterval(pollPortSweep, 1000);
  }).catch(err => {
    document.getElementById('portSweepResult').textContent = 'Could not start sweep: ' + err.message;
  });
};
const pollPortSweep = function(){
  fetch('/portsweepstatus')
    .then(r=>r.json())
    .then(d=>{
      let text = d.result;
      if (d.running) {
        text += '\n\n(' + Math.floor((Date.now() - portSweepPollStarted) / 1000) + 's elapsed)';
      }
      document.getElementById('portSweepResult').textContent = text;
      if (!d.running) { clearInterval(portSweepPoll); portSweepPoll = null; }
    })
    .catch(() => {
      clearInterval(portSweepPoll); portSweepPoll = null;
      document.getElementById('portSweepResult').textContent = 'Lost contact with the device.';
    });
};

const toggleStaticIpFields = function(){
  const show = document.getElementById('staticIpToggle').checked;
  document.getElementById('staticIpFields').style.display = show ? 'block' : 'none';
};
const saveNetworkSettings = function(){
  const enabled = document.getElementById('staticIpToggle').checked;
  if (enabled) {
    const ip = document.getElementById('staticIpAddr').value.trim();
    const subnet = document.getElementById('staticSubnetMask').value.trim();
    const gateway = document.getElementById('staticGateway').value.trim();
    if (!ip || !subnet || !gateway) {
      alert('IP address, subnet mask, and default gateway are required for a static IP.');
      return;
    }
  }
  const params = new URLSearchParams();
  params.set('enabled', enabled ? '1' : '0');
  params.set('ip', document.getElementById('staticIpAddr').value.trim());
  params.set('subnet', document.getElementById('staticSubnetMask').value.trim());
  params.set('gateway', document.getElementById('staticGateway').value.trim());
  params.set('dns1', document.getElementById('staticDns1').value.trim());
  params.set('dns2', document.getElementById('staticDns2').value.trim());
  document.getElementById('networkResult').textContent = 'Saving and rebooting...';
  fetch('/savenetwork?' + params.toString())
    .then(r=>r.text())
    .then(t=>{ document.getElementById('networkResult').textContent = t; });
};
fetch('/networkconfig')
  .then(r=>r.json())
  .then(d=>{
    document.getElementById('staticIpToggle').checked = d.enabled;
    document.getElementById('staticIpFields').style.display = d.enabled ? 'block' : 'none';
    document.getElementById('staticIpAddr').value = d.ip;
    document.getElementById('staticSubnetMask').value = d.subnet;
    document.getElementById('staticGateway').value = d.gateway;
    document.getElementById('staticDns1').value = d.dns1;
    document.getElementById('staticDns2').value = d.dns2;
    document.getElementById('networkResult').textContent = d.enabled ? 'Currently using a static IP.' : 'Currently using DHCP.';
  });

let blePoll = null;
const runBleScan = function(){
  document.getElementById('bleScanResult').textContent = 'Starting BLE scan...';
  document.getElementById('bleDeviceList').innerHTML = '';
  fetch('/blescan').then(r => {
    if (!r.ok) {
      return r.text().then(t => { throw new Error(t); });
    }
    if (blePoll) clearInterval(blePoll);
    blePoll = setInterval(pollBleScan, 700);
  }).catch(err => {
    document.getElementById('bleScanResult').textContent = 'Could not start scan: ' + err.message;
  });
};
const pollBleScan = function(){
  fetch('/blescanstatus')
    .then(r=>r.json())
    .then(d=>{
      document.getElementById('bleScanResult').textContent = d.result;
      const list = document.getElementById('bleDeviceList');
      list.innerHTML = '';
      d.devices.forEach(dev => {
        const row = document.createElement('div');
        row.style.cssText = 'padding:0.5rem;margin-bottom:0.4rem;border:1px solid var(--border);border-radius:6px;';
        const safeName = dev.name.replace(/'/g, "");
        row.innerHTML = '<strong>' + dev.name + '</strong> (' + dev.guess + ')<br>' + dev.mac + ' &middot; ' + dev.rssi + ' dBm' +
          ' <button onclick="startRadar(\'' + dev.mac + '\', \'' + safeName + '\')" style="margin-left:0.6rem;">Locate</button>';
        list.appendChild(row);
      });
      if (!d.running) {
        clearInterval(blePoll);
        blePoll = null;
      }
    })
    .catch(() => {
      clearInterval(blePoll);
      blePoll = null;
      document.getElementById('bleScanResult').textContent = 'Lost contact with the device.';
    });
};

let radarPoll = null;
const proximityLabel = function(rssi){
  if (rssi >= -50) return 'Very close';
  if (rssi >= -65) return 'Close';
  if (rssi >= -75) return 'Nearby';
  if (rssi >= -85) return 'Far';
  return 'Very far / weak signal';
};
const startRadar = function(mac, name){
  document.getElementById('bleListView').style.display = 'none';
  document.getElementById('bleRadarView').style.display = 'block';
  document.getElementById('radarTargetLabel').textContent = name + ' (' + mac + ')';
  document.getElementById('radarReadout').textContent = 'Searching...';
  document.getElementById('radarBlip').style.opacity = '0';
  fetch('/radarstart?mac=' + encodeURIComponent(mac)).then(() => {
    if (radarPoll) clearInterval(radarPoll);
    radarPoll = setInterval(pollRadar, 600);
    pollRadar();
  });
};
const stopRadar = function(){
  fetch('/radarstop');
  if (radarPoll) { clearInterval(radarPoll); radarPoll = null; }
  document.getElementById('bleRadarView').style.display = 'none';
  document.getElementById('bleListView').style.display = 'block';
};
const pollRadar = function(){
  fetch('/radarstatus')
    .then(r=>r.json())
    .then(d=>{
      const blip = document.getElementById('radarBlip');
      if (d.found) {
        const minRssi = -90, maxRssi = -40;
        const clamped = Math.max(minRssi, Math.min(maxRssi, d.rssi));
        const frac = (clamped - minRssi) / (maxRssi - minRssi);
        const radiusPercent = (1 - frac) * 45;
        blip.style.left = '50%';
        blip.style.top = (50 - radiusPercent) + '%';
        blip.style.opacity = '1';
        document.getElementById('radarReadout').textContent = d.rssi + ' dBm - ' + proximityLabel(d.rssi);
      } else {
        blip.style.opacity = '0';
        document.getElementById('radarReadout').textContent = 'Signal lost - move around to find it again.';
      }
    })
    .catch(() => {
      document.getElementById('radarReadout').textContent = 'Lost contact with the device.';
    });
};

const saveWifi = function(){
  const ssid = document.getElementById('wifiSsid').value.trim();
  const pass = document.getElementById('wifiPass').value.trim();
  const band = document.getElementById('band2gOnlyPref').checked ? '2g' : 'auto';
  if (!ssid) {
    document.getElementById('wifiResult').textContent = 'Saving band preference and rebooting...';
    fetch('/saveband?band=' + band)
      .then(r=>r.text())
      .then(t=>{
        document.getElementById('wifiResult').textContent = t;
      });
    return;
  }
  document.getElementById('wifiResult').textContent = 'Saving settings and rebooting...';
  fetch('/savewifi?ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass) + '&band=' + band)
    .then(r=>r.text())
    .then(t=>{
      document.getElementById('wifiResult').textContent = t;
    });
};

const loadNetStatus = function(){
  fetch('/netstatus').then(r=>r.json()).then(d=>{
    const el = document.getElementById('netStatusBanner');
    if (d.mode === 'sta') {
      el.innerHTML = 'Connected to <b>' + d.ssid + '</b> (' + d.band + ') &nbsp; RSSI: ' + d.rssi + ' dBm &nbsp; IP: ' + d.ip + ' &nbsp; (also at <b>' + d.hostname + '.local</b>)';
      const scan24Btn = document.getElementById('scan24Btn');
      const scan5Btn = document.getElementById('scan5Btn');
      if (d.band === '5GHz') {
        scan24Btn.disabled = true;
        scan24Btn.title = 'Switching to 2.4GHz while connected on 5GHz has reliably frozen the device - disabled.';
        scan5Btn.disabled = false;
        scan5Btn.title = '';
      } else {
        scan5Btn.disabled = true;
        scan5Btn.title = 'Switching to 5GHz while connected on 2.4GHz has reliably frozen the device - disabled.';
        scan24Btn.disabled = false;
        scan24Btn.title = '';
      }
    } else {
      let html = '<b>AP mode</b> (FortiGill) &mdash; ' + d.clients.length + ' client(s) connected';
      if (d.clients.length > 0) {
        html += '<table style="width:100%;margin-top:0.5rem;border-collapse:collapse;">';
        html += '<tr style="text-align:left;"><th>IP</th><th>MAC</th><th>Connected</th></tr>';
        d.clients.forEach(c => {
          const mins = Math.floor(c.connectedSec / 60);
          const secs = c.connectedSec % 60;
          html += '<tr><td>' + c.ip + '</td><td>' + c.mac + '</td><td>' + mins + 'm ' + secs + 's</td></tr>';
        });
        html += '</table>';
      }
      el.innerHTML = html;
    }
    if (!bandPrefLoaded) {
      document.getElementById('band5gPref').checked = !d.band2gOnly;
      document.getElementById('band2gOnlyPref').checked = d.band2gOnly;
      bandPrefLoaded = true;
    }
  });
};
setInterval(loadNetStatus, 3000);
loadNetStatus();

const toggleMeshFields = function(){
  const show = document.getElementById('meshToggle').checked;
  document.getElementById('meshFields').style.display = show ? 'block' : 'none';
};

const saveMeshSettings = function(){
  const enabled = document.getElementById('meshToggle').checked;
  const params = new URLSearchParams();
  params.set('enabled', enabled ? '1' : '0');
  params.set('nodeid', document.getElementById('meshNodeId').value.trim());
  params.set('meshname', document.getElementById('meshNameField').value.trim());
  params.set('meshpass', document.getElementById('meshPassField').value.trim());
  params.set('server', document.getElementById('meshServerField').value.trim());
  document.getElementById('meshResult').textContent = 'Saving mesh settings and rebooting...';
  fetch('/savemesh?' + params.toString())
    .then(r=>r.text())
    .then(t=>{
      document.getElementById('meshResult').textContent = t;
    });
};

const loadMeshStatus = function(){
  fetch('/meshconfig').then(r=>r.json()).then(d=>{
    document.getElementById('meshToggle').checked = d.enabled;
    document.getElementById('meshFields').style.display = d.enabled ? 'block' : 'none';
    document.getElementById('meshNodeId').value = d.nodeid;
    document.getElementById('meshNameField').value = d.meshname;
    document.getElementById('meshServerField').value = d.server;
    document.getElementById('meshResult').textContent = 'Settings save to flash and reboot the device.';
    // Mesh password intentionally not sent back by the server, so it's left blank here.
  });
};
loadMeshStatus();
</script>
</body></html>
)HTML";

void applyStaticIpIfEnabled() {
  if (!useStaticIp) return;
  IPAddress ip, gw, sn, d1, d2;
  bool validStatic = ip.fromString(staticIp) && gw.fromString(staticGateway) && sn.fromString(staticSubnet);
  if (validStatic) {
    if (staticDns1.length() == 0 || !d1.fromString(staticDns1)) d1 = IPAddress(0, 0, 0, 0);
    if (staticDns2.length() == 0 || !d2.fromString(staticDns2)) d2 = IPAddress(0, 0, 0, 0);
    WiFi.config(ip, gw, sn, d1, d2);
    Serial.println("Static IP configured: " + ip.toString());
  } else {
    Serial.println("Static IP settings invalid - falling back to DHCP.");
  }
}

String guessBleDeviceType(const String &name, int companyId, int appleAdvType, bool hasFastPair, bool hasHeartRate, bool hasHid, bool isDrone) {
  if (isDrone) return "Drone (Remote ID / ASTM F3411 broadcast)";

  String nameUpper = name;
  nameUpper.toUpperCase();

  // Name-based hints are the most direct signal when a device offers one.
  if (nameUpper.indexOf("IPHONE") >= 0) return "Apple iPhone";
  if (nameUpper.indexOf("IPAD") >= 0) return "Apple iPad";
  if (nameUpper.indexOf("MACBOOK") >= 0 || nameUpper.indexOf("IMAC") >= 0) return "Apple Mac";
  if (nameUpper.indexOf("APPLE WATCH") >= 0) return "Apple Watch";
  if (nameUpper.indexOf("AIRPODS") >= 0) return "Apple AirPods";
  if (nameUpper.indexOf("GALAXY") >= 0 || nameUpper.indexOf("PIXEL") >= 0 || nameUpper.indexOf("ONEPLUS") >= 0 || nameUpper.indexOf("XIAOMI") >= 0 || nameUpper.indexOf("REDMI") >= 0) return "Android phone";
  if (nameUpper.indexOf("FITBIT") >= 0) return "Fitbit wearable";
  if (nameUpper.indexOf("GARMIN") >= 0) return "Garmin wearable";
  if (nameUpper.indexOf("MI BAND") >= 0) return "Xiaomi Mi Band";

  // Service UUIDs: Fast Pair (0xFE2C) shows up on most Android phones and accessories.
  if (hasFastPair) return "Android (Fast Pair)";
  if (hasHeartRate) return "Heart rate monitor";
  if (hasHid) return "HID device (keyboard/mouse/remote)";

  if (companyId == 0x004C) {  // Apple
    if (appleAdvType == 0x12) {
      // Apple "Find My" network accessory broadcast -- re-validated
      // against real capture data: a confirmed iPhone showed type 0x10
      // (ordinary "Nearby" continuity) instead, and only 1 of 6 nearby
      // Apple devices in a real test scan showed 0x12. Shared by
      // AirTags, AirPods, and other Find My-compatible trackers, so it
      // can't be narrowed to "AirTag" specifically from this byte alone.
      return "Apple Find My accessory (AirTag, AirPods, or similar tracker)";
    }
    if (appleAdvType == 0x07) {
      // Apple "Proximity Pairing" message -- documented specifically as
      // the AirPods/audio-accessory pairing broadcast (furiousMAC
      // continuity project), distinct from ordinary Nearby (0x10).
      // Only sent around pairing events (e.g. case lid opened), not
      // continuously.
      return "Apple AirPods (or similar audio accessory)";
    }
    // NOTE: no verified, documented way found to identify "iPhone"
    // specifically (vs iPad/Mac/Watch) from the ordinary Nearby (0x10)
    // broadcast -- checked furiousMAC's continuity docs and a real
    // open-source parser, neither defines a device-type field for it.
    // Staying generic here rather than guessing.
    if (name.length() > 0) return "Apple device (" + name + ")";
    return "Apple device (iPhone/iPad/Mac/Watch)";
  }
  if (companyId == 0x00E0) return "Google/Android device";
  if (companyId == 0x0075) return "Samsung device";
  if (companyId == 0x0157) return "Fitbit device";
  if (companyId == 0x0087) return "Garmin device";
  if (companyId == 0x0006) return "Microsoft device";
  if (companyId == 0x0059) return "Nordic Semiconductor (generic BLE board)";

  if (name.length() > 0) return "Unknown device (" + name + ")";
  return "Unknown BLE device";
}

static int bleGenericGapHandler(struct ble_gap_event *event, void *arg) {
  if (event->type != BLE_GAP_EVENT_EXT_DISC) {
    return 0;
  }
  const struct ble_gap_ext_disc_desc &d = event->ext_disc;

  char macBuf[18];
  snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
           d.addr.val[5], d.addr.val[4], d.addr.val[3],
           d.addr.val[2], d.addr.val[1], d.addr.val[0]);
  String mac = String(macBuf);

  int idx = bleGenericFindOrAddDevice(mac);
  if (idx < 0) return 0;
  bleGenericDevices[idx].rssi = (int)d.rssi;
  bleGenericDevices[idx].lastSeenMs = millis();

  int i = 0;
  while (i < d.length_data) {
    uint8_t adLen = d.data[i];
    if (adLen == 0) break;
    if (i + 1 + adLen > d.length_data) break;
    uint8_t adType = d.data[i + 1];

    if ((adType == 0x08 || adType == 0x09) && adLen >= 2) {
      String nm = "";
      for (int k = 0; k < adLen - 1; k++) nm += (char)d.data[i + 2 + k];
      if (adType == 0x09 || bleGenericDevices[idx].name.length() == 0) {
        bleGenericDevices[idx].name = nm;
      }
    } else if ((adType == 0x02 || adType == 0x03) && adLen >= 3) {
      int numUuidBytes = adLen - 1;
      for (int k = 0; k + 2 <= numUuidBytes; k += 2) {
        uint16_t uuid = d.data[i + 2 + k] | (d.data[i + 3 + k] << 8);
        if (uuid == 0xFE2C) bleGenericDevices[idx].hasFastPair = true;
        if (uuid == 0x180D) bleGenericDevices[idx].hasHeartRate = true;
        if (uuid == 0x1812) bleGenericDevices[idx].hasHid = true;
      }
    } else if (adType == 0xFF && adLen >= 3) {
      bleGenericDevices[idx].companyId = d.data[i + 2] | (d.data[i + 3] << 8);
      if (bleGenericDevices[idx].companyId == 0x004C && adLen >= 4) {
        bleGenericDevices[idx].appleAdvType = d.data[i + 4];
      }
      // Dump the full manufacturer-specific payload as hex for diagnostics --
      // this is what lets us fingerprint Apple message types, Tile, and
      // Samsung SmartTag correctly later instead of guessing from memory.
      String hex = "";
      for (int k = 0; k < adLen - 1; k++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02X", d.data[i + 2 + k]);
        hex += buf;
      }
      bleGenericDevices[idx].mfgDataHex = hex;
    } else if (adType == 0x16 && adLen >= 3) {
      uint16_t svcUuid = d.data[i + 2] | (d.data[i + 3] << 8);
      if (svcUuid == 0xFFFA) {
        bleGenericDevices[idx].isDrone = true;
      }
    } else if (adType == 0x19 && adLen >= 3) {
      // GAP Appearance -- diagnostics only for now, see note at top of
      // this patch script before attaching any classification to it.
      bleGenericDevices[idx].appearance = d.data[i + 2] | (d.data[i + 3] << 8);
    }

    i += 1 + adLen;
  }
  return 0;
}

void bleScanTask(void *param) {
  bleGenericDeviceCount = 0;

  struct ble_gap_ext_disc_params bleGenParams = {};
  bleGenParams.itvl = 64;
  bleGenParams.window = 64;
  bleGenParams.passive = 0;
  bleGenParams.disable_observer_mode = 0;
  struct ble_gap_ext_disc_params bleGenCodedParams = bleGenParams;

  Serial.println("Starting BLE scan...");
  int rc = ble_gap_ext_disc(BLE_OWN_ADDR_PUBLIC, 0, 0, 0,
                             BLE_HCI_SCAN_FILT_NO_WL, 0,
                             &bleGenParams, &bleGenCodedParams,
                             bleGenericGapHandler, NULL);
  if (rc != 0) {
    Serial.println("[BLE scan debug] ble_gap_ext_disc() failed, rc=" + String(rc));
  }

  delay(8000);
  ble_gap_disc_cancel();

  int count = bleGenericDeviceCount;
  String result = "Found " + String(count) + " BLE device" + (count == 1 ? "" : "s") + ":\n\n";
  String devicesJson = "[";

  for (int i = 0; i < count; i++) {
    String name = bleGenericDevices[i].name.length() > 0 ? bleGenericDevices[i].name : "(no name)";
    String mac = bleGenericDevices[i].mac;
    int rssi = bleGenericDevices[i].rssi;
    String guess = guessBleDeviceType(bleGenericDevices[i].name, bleGenericDevices[i].companyId,
                                       bleGenericDevices[i].appleAdvType, bleGenericDevices[i].hasFastPair,
                                       bleGenericDevices[i].hasHeartRate, bleGenericDevices[i].hasHid,
                                       bleGenericDevices[i].isDrone);

    result += name + "\n";
    result += "  MAC: " + mac + "\n";
    result += "  RSSI: " + String(rssi) + " dBm\n";
    result += "  Guess: " + guess + "\n\n";

    Serial.println("Found: " + name + " (" + mac + ") RSSI " + String(rssi) + " - " + guess);
    if (bleGenericDevices[i].mfgDataHex.length() > 0) {
      Serial.println("  MfgData: " + bleGenericDevices[i].mfgDataHex +
                      " (companyId=0x" + String(bleGenericDevices[i].companyId, HEX) + ")");
    }
    if (bleGenericDevices[i].appearance >= 0) {
      int category = bleGenericDevices[i].appearance >> 6;
      int sub = bleGenericDevices[i].appearance & 0x3F;
      Serial.println("  Appearance: 0x" + String(bleGenericDevices[i].appearance, HEX) +
                      " (category=0x" + String(category, HEX) + ", sub=0x" + String(sub, HEX) + ")");
    }

    if (i > 0) devicesJson += ",";
    devicesJson += "{\"name\":\"" + jsonEscapeForWifiScan(name) + "\",\"mac\":\"" + mac + "\",\"rssi\":" + String(rssi) + ",\"guess\":\"" + jsonEscapeForWifiScan(guess) + "\"}";
  }
  devicesJson += "]";

  bleGenericDeviceCount = 0;

  bleScanDevicesJson = devicesJson;
  bleScanTaskResult = result;
  bleScanTaskRunning = false;
  Serial.println("BLE scan complete.");
  vTaskDelete(NULL);
}

static int radarGapHandler(struct ble_gap_event *event, void *arg) {
  if (event->type != BLE_GAP_EVENT_EXT_DISC) {
    return 0;
  }
  const struct ble_gap_ext_disc_desc &d = event->ext_disc;
  char macBuf[18];
  snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
           d.addr.val[5], d.addr.val[4], d.addr.val[3],
           d.addr.val[2], d.addr.val[1], d.addr.val[0]);
  String mac = String(macBuf);
  if (mac.equalsIgnoreCase(radarTargetMac)) {
    radarLastRssi = (int)d.rssi;
    radarLastSeenMs = millis();
  }
  return 0;
}

void radarTask(void *param) {
  struct ble_gap_ext_disc_params radarParams = {};
  radarParams.itvl = 64;
  radarParams.window = 64;
  radarParams.passive = 0;
  radarParams.disable_observer_mode = 0;
  struct ble_gap_ext_disc_params radarCodedParams = radarParams;

  int rc = ble_gap_ext_disc(BLE_OWN_ADDR_PUBLIC, 0, 0, 0,
                             BLE_HCI_SCAN_FILT_NO_WL, 0,
                             &radarParams, &radarCodedParams,
                             radarGapHandler, NULL);
  if (rc != 0) {
    Serial.println("[Radar debug] ble_gap_ext_disc() failed, rc=" + String(rc));
  }

  while (radarActive) {
    delay(200);
  }
  ble_gap_disc_cancel();
  vTaskDelete(NULL);
}

String decodeDnsName(uint8_t *packet, int packetLen, int startOffset) {
  String name = "";
  int offset = startOffset;
  int jumps = 0;
  while (offset >= 0 && offset < packetLen && jumps < 10) {
    uint8_t lbl = packet[offset];
    if (lbl == 0) {
      break;
    }
    if ((lbl & 0xC0) == 0xC0) {
      if (offset + 1 >= packetLen) break;
      int pointer = ((lbl & 0x3F) << 8) | packet[offset + 1];
      offset = pointer;
      jumps++;
      continue;
    }
    offset++;
    for (int i = 0; i < lbl && offset < packetLen; i++) {
      name += (char)packet[offset++];
    }
    name += ".";
  }
  if (name.endsWith(".")) name.remove(name.length() - 1);
  return name;
}

String reverseDnsLookup(IPAddress ip, int timeoutMs) {
  IPAddress dnsServer = WiFi.dnsIP(0);
  if (dnsServer[0] == 0 && dnsServer[1] == 0 && dnsServer[2] == 0 && dnsServer[3] == 0) {
    return "";
  }

  WiFiUDP udp;
  if (!udp.begin(0)) return "";

  uint8_t queryBuf[64];
  uint16_t queryId = (uint16_t)millis();
  int pos = 0;
  queryBuf[pos++] = (queryId >> 8) & 0xFF;
  queryBuf[pos++] = queryId & 0xFF;
  queryBuf[pos++] = 0x01;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x01;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x00;

  for (int i = 3; i >= 0; i--) {
    String octet = String(ip[i]);
    queryBuf[pos++] = octet.length();
    for (unsigned int j = 0; j < octet.length(); j++) queryBuf[pos++] = octet[j];
  }
  queryBuf[pos++] = 7;
  memcpy(queryBuf + pos, "in-addr", 7);
  pos += 7;
  queryBuf[pos++] = 4;
  memcpy(queryBuf + pos, "arpa", 4);
  pos += 4;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x0C;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x01;

  int questionLen = pos - 12;

  udp.beginPacket(dnsServer, 53);
  udp.write(queryBuf, pos);
  udp.endPacket();

  unsigned long startWait = millis();
  int packetSize = 0;
  while (millis() - startWait < (unsigned long)timeoutMs) {
    packetSize = udp.parsePacket();
    if (packetSize > 0) break;
    delay(10);
  }

  if (packetSize <= 0) {
    udp.stop();
    return "";
  }

  uint8_t respBuf[256];
  int respLen = udp.read(respBuf, min(packetSize, (int)sizeof(respBuf)));
  udp.stop();

  if (respLen < 12) return "";

  int ancount = (respBuf[6] << 8) | respBuf[7];
  if (ancount <= 0) return "";

  int rpos = 12 + questionLen;
  String hostname = "";
  for (int i = 0; i < ancount && rpos < respLen - 10; i++) {
    if ((respBuf[rpos] & 0xC0) == 0xC0) {
      rpos += 2;
    } else {
      while (rpos < respLen && respBuf[rpos] != 0) {
        rpos += respBuf[rpos] + 1;
      }
      rpos += 1;
    }
    uint16_t rtype = (respBuf[rpos] << 8) | respBuf[rpos + 1];
    rpos += 2;
    rpos += 2;  // class
    rpos += 4;  // ttl
    uint16_t rdlen = (respBuf[rpos] << 8) | respBuf[rpos + 1];
    rpos += 2;
    if (rtype == 12) {  // PTR
      hostname = decodeDnsName(respBuf, respLen, rpos);
      break;
    }
    rpos += rdlen;
  }
  return hostname;
}

void traceTask(void *param) {
  String target = traceTargetInput;
  int maxHops = traceMaxHopsInput;
  int timeoutMs = traceTimeoutInput;

  struct addrinfo hint;
  memset(&hint, 0, sizeof(hint));
  hint.ai_family = AF_INET;
  struct addrinfo *res = NULL;

  int gaierr = getaddrinfo(target.c_str(), NULL, &hint, &res);
  if (gaierr != 0 || res == NULL) {
    traceTaskResult = "Could not resolve " + target;
    traceTaskRunning = false;
    vTaskDelete(NULL);
    return;
  }
  struct in_addr destIn = ((struct sockaddr_in *)(res->ai_addr))->sin_addr;
  freeaddrinfo(res);

  struct sockaddr_in destAddr;
  memset(&destAddr, 0, sizeof(destAddr));
  destAddr.sin_family = AF_INET;
  destAddr.sin_addr = destIn;

  int sock = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
  if (sock < 0) {
    traceTaskResult = "Could not open a raw socket for traceroute.";
    traceTaskRunning = false;
    vTaskDelete(NULL);
    return;
  }

  struct timeval tv;
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  const int payloadSize = 32;
  const int packetSize = sizeof(struct icmp_echo_hdr) + payloadSize;
  uint8_t packetBuf[packetSize];
  uint16_t traceId = (uint16_t)(xTaskGetTickCount() & 0xFFFF);

  String result = "Tracing route to " + target + ", max " + String(maxHops) + " hops:\n\n";
  traceTaskResult = result;
  Serial.println("Starting traceroute to " + target);

  bool reachedTarget = false;

  for (int ttl = 1; ttl <= maxHops && !reachedTarget; ttl++) {
    setsockopt(sock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));

    memset(packetBuf, 0, packetSize);
    struct icmp_echo_hdr *header = (struct icmp_echo_hdr *)packetBuf;
    header->type = ICMP_ECHO;
    header->code = 0;
    header->id = traceId;
    header->seqno = ttl;
    for (int i = 0; i < payloadSize; i++) {
      packetBuf[sizeof(struct icmp_echo_hdr) + i] = 'A' + (i % 26);
    }
    header->chksum = 0;
    header->chksum = inet_chksum(packetBuf, packetSize);

    unsigned long sentAt = millis();
    sendto(sock, packetBuf, packetSize, 0, (struct sockaddr *)&destAddr, sizeof(destAddr));

    char recvBuf[128];
    struct sockaddr_in fromAddr;
    socklen_t fromLen = sizeof(fromAddr);
    int len = recvfrom(sock, recvBuf, sizeof(recvBuf), 0, (struct sockaddr *)&fromAddr, &fromLen);

    String hopLine = String(ttl) + "  ";

    if (len <= 0) {
      hopLine += "* (no response)";
    } else {
      unsigned long rtt = millis() - sentAt;
      struct ip_hdr *replyIpHdr = (struct ip_hdr *)recvBuf;
      int ipHdrLen = IPH_HL_BYTES(replyIpHdr);
      struct icmp_echo_hdr *replyIcmp = (struct icmp_echo_hdr *)(recvBuf + ipHdrLen);
      ip4_addr_t fromIp4;
      inet_addr_to_ip4addr(&fromIp4, &fromAddr.sin_addr);
      char ipStr[16];
      ip4addr_ntoa_r(&fromIp4, ipStr, sizeof(ipStr));

      String hopHostname = "";
      IPAddress hopIp;
      if (hopIp.fromString(ipStr)) {
        hopHostname = reverseDnsLookup(hopIp, 500);
      }
      String hostSuffix = hopHostname.length() > 0 ? (" (" + hopHostname + ")") : "";

      if (replyIcmp->type == 11) {  // Time Exceeded - an intermediate router answered
        hopLine += String(ipStr) + hostSuffix + "  " + String(rtt) + " ms";
      } else if (replyIcmp->type == 0) {  // Echo Reply - we reached the destination
        hopLine += String(ipStr) + hostSuffix + "  " + String(rtt) + " ms  (destination reached)";
        reachedTarget = true;
      } else {
        hopLine += String(ipStr) + hostSuffix + "  (ICMP type " + String(replyIcmp->type) + ")";
      }
    }

    Serial.println(hopLine);
    result += hopLine + "\n";
    traceTaskResult = result;
  }

  if (!reachedTarget) {
    result += "\nMax hops reached without a reply from the destination.\n";
    traceTaskResult = result;
  }

  close(sock);
  traceTaskRunning = false;
  Serial.println("Traceroute complete.");
  vTaskDelete(NULL);
}

String dnsQuery(IPAddress dnsServer, const String &qname, uint16_t qtype, int timeoutMs) {
  WiFiUDP udp;
  if (!udp.begin(0)) return "(could not open local UDP socket)\n";

  uint8_t queryBuf[160];
  uint16_t queryId = (uint16_t)millis();
  int pos = 0;
  queryBuf[pos++] = (queryId >> 8) & 0xFF;
  queryBuf[pos++] = queryId & 0xFF;
  queryBuf[pos++] = 0x01;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x01;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x00;

  int start = 0;
  while (start < (int)qname.length()) {
    int dot = qname.indexOf('.', start);
    if (dot == -1) dot = qname.length();
    int labelLen = dot - start;
    queryBuf[pos++] = (uint8_t)labelLen;
    for (int i = start; i < dot; i++) queryBuf[pos++] = qname[i];
    start = dot + 1;
  }
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = (qtype >> 8) & 0xFF;
  queryBuf[pos++] = qtype & 0xFF;
  queryBuf[pos++] = 0x00;
  queryBuf[pos++] = 0x01;

  int questionLen = pos - 12;

  udp.beginPacket(dnsServer, 53);
  udp.write(queryBuf, pos);
  udp.endPacket();

  unsigned long startWait = millis();
  int packetSize = 0;
  while (millis() - startWait < (unsigned long)timeoutMs) {
    packetSize = udp.parsePacket();
    if (packetSize > 0) break;
    delay(10);
  }

  if (packetSize <= 0) {
    udp.stop();
    return "(no response)\n";
  }

  uint8_t respBuf[512];
  int respLen = udp.read(respBuf, min(packetSize, (int)sizeof(respBuf)));
  udp.stop();

  if (respLen < 12) return "(bad response)\n";

  uint16_t rcode = respBuf[3] & 0x0F;
  if (rcode == 3) return "(not found - NXDOMAIN)\n";
  if (rcode != 0) return "(DNS server returned error code " + String(rcode) + ")\n";

  int ancount = (respBuf[6] << 8) | respBuf[7];
  if (ancount <= 0) return "(no records found)\n";

  String result = "";
  int rpos = 12 + questionLen;
  for (int i = 0; i < ancount && rpos < respLen - 10; i++) {
    if ((respBuf[rpos] & 0xC0) == 0xC0) {
      rpos += 2;
    } else {
      while (rpos < respLen && respBuf[rpos] != 0) {
        rpos += respBuf[rpos] + 1;
      }
      rpos += 1;
    }
    uint16_t rtype = (respBuf[rpos] << 8) | respBuf[rpos + 1];
    rpos += 2;
    rpos += 2;  // class
    rpos += 4;  // ttl
    uint16_t rdlen = (respBuf[rpos] << 8) | respBuf[rpos + 1];
    rpos += 2;
    int rdataStart = rpos;

    if (rtype == 1 && rdlen == 4) {  // A
      result += String(respBuf[rdataStart]) + "." + String(respBuf[rdataStart + 1]) + "." + String(respBuf[rdataStart + 2]) + "." + String(respBuf[rdataStart + 3]) + "\n";
    } else if (rtype == 5 || rtype == 2 || rtype == 12) {  // CNAME, NS, PTR
      result += decodeDnsName(respBuf, respLen, rdataStart) + "\n";
    } else if (rtype == 15 && rdlen >= 3) {  // MX
      uint16_t prio = (respBuf[rdataStart] << 8) | respBuf[rdataStart + 1];
      String mxName = decodeDnsName(respBuf, respLen, rdataStart + 2);
      result += "priority " + String(prio) + ": " + mxName + "\n";
    } else if (rtype == 16) {  // TXT
      int tpos = rdataStart;
      String txt = "";
      while (tpos < rdataStart + rdlen && tpos < respLen) {
        uint8_t tlen = respBuf[tpos++];
        for (int j = 0; j < tlen && tpos < respLen; j++) txt += (char)respBuf[tpos++];
      }
      result += "\"" + txt + "\"\n";
    } else {
      result += "(record type " + String(rtype) + ")\n";
    }
    rpos = rdataStart + rdlen;
  }
  if (result.length() == 0) result = "(no matching records)\n";
  return result;
}

void dnsLookupTask(void *param) {
  String target = dnsLookupInput;
  IPAddress dnsServer = WiFi.dnsIP(0);

  if (dnsServer[0] == 0 && dnsServer[1] == 0 && dnsServer[2] == 0 && dnsServer[3] == 0) {
    dnsLookupResult = "No DNS server is configured on this device.";
    dnsLookupRunning = false;
    vTaskDelete(NULL);
    return;
  }

  String result = "";
  IPAddress asIp;
  if (asIp.fromString(target)) {
    String qname = String(asIp[3]) + "." + String(asIp[2]) + "." + String(asIp[1]) + "." + String(asIp[0]) + ".in-addr.arpa";
    result = "PTR (reverse) for " + target + ":\n" + dnsQuery(dnsServer, qname, 12, 1500);
  } else {
    result += "A records for " + target + ":\n" + dnsQuery(dnsServer, target, 1, 1500) + "\n";
    result += "CNAME for " + target + ":\n" + dnsQuery(dnsServer, target, 5, 1500) + "\n";
    result += "NS records for " + target + ":\n" + dnsQuery(dnsServer, target, 2, 1500) + "\n";
    result += "MX records for " + target + ":\n" + dnsQuery(dnsServer, target, 15, 1500) + "\n";
    result += "TXT records for " + target + ":\n" + dnsQuery(dnsServer, target, 16, 1500) + "\n";
  }

  dnsLookupResult = result;
  dnsLookupRunning = false;
  Serial.println("DNS lookup complete for " + target);
  vTaskDelete(NULL);
}

String whoisQuery(const String &server, const String &target, int timeoutMs) {
  WiFiClient client;
  if (!client.connect(server.c_str(), 43, timeoutMs)) {
    return "";
  }
  client.print(target + "\r\n");

  String response = "";
  unsigned long start = millis();
  while ((client.connected() || client.available()) && millis() - start < (unsigned long)timeoutMs) {
    while (client.available()) {
      char c = client.read();
      response += c;
      if (response.length() > 6000) break;
    }
    if (response.length() > 6000) break;
    delay(10);
  }
  client.stop();
  return response;
}

void whoisTask(void *param) {
  String target = whoisInput;

  Serial.println("Starting WHOIS for " + target);
  String iana = whoisQuery("whois.iana.org", target, 5000);
  if (iana.length() == 0) {
    whoisTaskResult = "Could not reach whois.iana.org.";
    whoisTaskRunning = false;
    vTaskDelete(NULL);
    return;
  }

  String referServer = "";
  int searchPos = 0;
  while (true) {
    int lineEnd = iana.indexOf('\n', searchPos);
    String line = (lineEnd == -1) ? iana.substring(searchPos) : iana.substring(searchPos, lineEnd);
    String lineLower = line;
    lineLower.toLowerCase();
    if (lineLower.startsWith("refer:") || lineLower.startsWith("whois:")) {
      int colon = line.indexOf(':');
      referServer = line.substring(colon + 1);
      referServer.trim();
      break;
    }
    if (lineEnd == -1) break;
    searchPos = lineEnd + 1;
  }

  String result;
  if (referServer.length() > 0) {
    Serial.println("WHOIS referral to " + referServer);
    String finalResult = whoisQuery(referServer, target, 5000);
    if (finalResult.length() > 0) {
      result = "(via " + referServer + ")\n\n" + finalResult;
    } else {
      result = "(referral to " + referServer + " failed - showing IANA response instead)\n\n" + iana;
    }
  } else {
    result = iana;
  }

  whoisTaskResult = result;
  whoisTaskRunning = false;
  Serial.println("WHOIS complete for " + target);
  vTaskDelete(NULL);
}

void reverseDnsTask(void *param) {
  String target = reverseDnsInput;
  IPAddress asIp;
  if (!asIp.fromString(target)) {
    reverseDnsResult = "Enter a valid IP address for a reverse DNS lookup.";
    reverseDnsRunning = false;
    vTaskDelete(NULL);
    return;
  }

  IPAddress dnsServer = WiFi.dnsIP(0);
  if (dnsServer[0] == 0 && dnsServer[1] == 0 && dnsServer[2] == 0 && dnsServer[3] == 0) {
    reverseDnsResult = "No DNS server is configured on this device.";
    reverseDnsRunning = false;
    vTaskDelete(NULL);
    return;
  }

  String qname = String(asIp[3]) + "." + String(asIp[2]) + "." + String(asIp[1]) + "." + String(asIp[0]) + ".in-addr.arpa";
  String result = dnsQuery(dnsServer, qname, 12, 1500);
  reverseDnsResult = "PTR (reverse) for " + target + ":\n" + result;
  reverseDnsRunning = false;
  Serial.println("Reverse DNS complete for " + target);
  vTaskDelete(NULL);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.onEvent(onWifiEvent);

  preferences.begin("net-tools", false);
  savedSsid = preferences.getString("ssid", "");
  savedPass = preferences.getString("pass", "");
  rememberLedPref = preferences.getBool("rememberLed", false);
  bool savedLedOff = preferences.getBool("ledOff", false);
  band2gOnly = preferences.getBool("band2gOnly", false);
  useStaticIp = preferences.getBool("staticIpOn", false);
  staticIp = preferences.getString("staticIp", "");
  staticSubnet = preferences.getString("staticSubnet", "");
  staticGateway = preferences.getString("staticGw", "");
  staticDns1 = preferences.getString("staticDns1", "");
  staticDns2 = preferences.getString("staticDns2", "");
  meshModeEnabled = preferences.getBool("meshOn", false);
  meshNodeId = preferences.getString("meshNodeId", "");
  meshName = preferences.getString("meshName", "");
  meshPassword = preferences.getString("meshPass", "");
  meshServerAddr = preferences.getString("meshServer", "");

  rgbLed.begin();
  rgbLed.setBrightness(80);
  rgbLed.setPixelColor(0, rgbLed.Color(60, 0, 0));  // solid red: booting up
  rgbLed.show();

  BLEDevice::init("ESP32-C5 Net Tools");

  {
    temperature_sensor_config_t tempSensorConfig = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
    if (temperature_sensor_install(&tempSensorConfig, &tempSensorHandle) == ESP_OK) {
      if (temperature_sensor_enable(tempSensorHandle) == ESP_OK) {
        tempSensorOk = true;
      }
    }
  }

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
  } else {
    Serial.println("SPIFFS mounted: " + String(SPIFFS.usedBytes()) + "/" + String(SPIFFS.totalBytes()) + " bytes used");
  }

  bool connected = false;

  if (savedSsid.length() > 0) {
    WiFi.mode(WIFI_STA);

    if (band2gOnly) {
      Serial.println("Locked to 2.4GHz only - skipping the 5GHz scan.");
      WiFi.setBandMode(WIFI_BAND_MODE_2G_ONLY);
      delay(150);
      int n2g = WiFi.scanNetworks(false, false, false, 300, 0, savedSsid.c_str());
      if (n2g > 0) {
        wifi_ap_record_t *rec = (wifi_ap_record_t *)WiFi.getScanInfoByIndex(0);
        Serial.println("Found on 2.4GHz, RSSI " + String(rec->rssi) + " dBm");
        applyStaticIpIfEnabled();
        WiFi.begin(savedSsid.c_str(), savedPass.c_str(), rec->primary, rec->bssid);
      } else {
        Serial.println("Didn't see the saved network on 2.4GHz during the scan - trying a normal connect anyway.");
        applyStaticIpIfEnabled();
        WiFi.begin(savedSsid.c_str(), savedPass.c_str());
      }
      WiFi.scanDelete();
    } else {
      Serial.println("Checking which band has the saved network...");
      bool found2g = false, found5g = false;
      int32_t rssi2g = -127, rssi5g = -127;
      uint8_t bssid2g[6] = {0};
      uint8_t bssid5g[6] = {0};
      int32_t chan2g = 0, chan5g = 0;

      WiFi.setBandMode(WIFI_BAND_MODE_2G_ONLY);
      delay(150);
      int n2g = WiFi.scanNetworks(false, false, false, 300, 0, savedSsid.c_str());
      if (n2g > 0) {
        wifi_ap_record_t *rec = (wifi_ap_record_t *)WiFi.getScanInfoByIndex(0);
        found2g = true;
        rssi2g = rec->rssi;
        memcpy(bssid2g, rec->bssid, 6);
        chan2g = rec->primary;
        Serial.println("Found on 2.4GHz, RSSI " + String(rssi2g) + " dBm");
      }
      WiFi.scanDelete();

      WiFi.setBandMode(WIFI_BAND_MODE_5G_ONLY);
      delay(150);
      int n5g = WiFi.scanNetworks(false, false, false, 300, 0, savedSsid.c_str());
      if (n5g > 0) {
        wifi_ap_record_t *rec = (wifi_ap_record_t *)WiFi.getScanInfoByIndex(0);
        found5g = true;
        rssi5g = rec->rssi;
        memcpy(bssid5g, rec->bssid, 6);
        chan5g = rec->primary;
        Serial.println("Found on 5GHz, RSSI " + String(rssi5g) + " dBm");
      }
      WiFi.scanDelete();

      bool use5g = found5g && (!found2g || rssi5g >= rssi2g - 12);

      if (use5g) {
        Serial.println("Preferring 5GHz - connecting there.");
        WiFi.setBandMode(WIFI_BAND_MODE_5G_ONLY);
        applyStaticIpIfEnabled();
        WiFi.begin(savedSsid.c_str(), savedPass.c_str(), chan5g, bssid5g);
      } else if (found2g) {
        Serial.println("Using 2.4GHz.");
        WiFi.setBandMode(WIFI_BAND_MODE_2G_ONLY);
        applyStaticIpIfEnabled();
        WiFi.begin(savedSsid.c_str(), savedPass.c_str(), chan2g, bssid2g);
      } else {
        Serial.println("Didn't see the saved network on either band during the scan - trying a normal connect anyway.");
        WiFi.setBandMode(WIFI_BAND_MODE_AUTO);
        applyStaticIpIfEnabled();
        WiFi.begin(savedSsid.c_str(), savedPass.c_str());
      }
    }

    Serial.print("Connecting to WiFi: ");
    Serial.println(savedSsid);

    int attempts = 0;
    bool redBlinkState = false;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      for (int b = 0; b < 5; b++) {
        redBlinkState = !redBlinkState;
        rgbLed.setPixelColor(0, redBlinkState ? rgbLed.Color(60, 0, 0) : 0);  // blinking red: connecting
        rgbLed.show();
        delay(100);
      }
      Serial.print(".");
      attempts++;
    }
    Serial.println();
    connected = (WiFi.status() == WL_CONNECTED);
    if (connected) {
      lastConnectedMs = millis();
      if (!band2gOnly && WiFi.getBand() == WIFI_BAND_5G) {
        connectedVia5g = true;
        Serial.println("Connected on 5GHz - will fall back to 2.4GHz if this connection is lost for too long.");
      }
    }
  } else {
    Serial.println("No saved WiFi network. Starting setup AP...");
  }

  if (connected) {
    isApMode = false;
    Serial.print("Connected! IP address: ");
    Serial.println(WiFi.localIP());

    if (rememberLedPref) {
      rgbLedOn = !savedLedOff;
    }

    String mdnsHostname = (meshModeEnabled && meshNodeId.length() > 0) ? ("esp32-c5-" + meshNodeId) : "esp32-c5";
    if (MDNS.begin(mdnsHostname.c_str())) {
      MDNS.addService("http", "tcp", 80);
      Serial.println("mDNS responder started: http://" + mdnsHostname + ".local");
    } else {
      Serial.println("Error starting mDNS responder");
    }
  } else {
    if (savedSsid.length() > 0) {
      Serial.println("Failed to connect to saved network. Starting setup AP...");
    }
    WiFi.mode(WIFI_AP);
    isApMode = true;
    WiFi.softAP("FortiGill", "fortigillwins");
    dnsServer.start(53, "*", WiFi.softAPIP());
    Serial.print("Setup AP IP address: ");
    Serial.println(WiFi.softAPIP());
    Serial.println("Connect to \"FortiGill\" and visit http://192.168.4.1 to configure your network.");
  }

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    const size_t totalLen = strlen_P(INDEX_HTML);
    AsyncWebServerResponse *response = req->beginChunkedResponse("text/html",
      [totalLen](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        if (index >= totalLen) return 0;
        size_t toCopy = (maxLen < (totalLen - index)) ? maxLen : (totalLen - index);
        memcpy_P(buffer, INDEX_HTML + index, toCopy);
        return toCopy;
      });
    req->send(response);
  });

  server.on("/savewifi", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("ssid")) {
      req->send(400, "text/plain", "missing ssid param");
      return;
    }
    String newSsid = req->getParam("ssid")->value();
    String newPass = req->hasParam("pass") ? req->getParam("pass")->value() : "";
    bool newBand2gOnly = req->hasParam("band") && req->getParam("band")->value() == "2g";

    preferences.putString("ssid", newSsid);
    preferences.putString("pass", newPass);
    preferences.putBool("band2gOnly", newBand2gOnly);

    req->send(200, "text/plain", "Saved successfully! Rebooting device now...");
    delay(1000);
    ESP.restart();
  });

  server.on("/saveband", HTTP_GET, [](AsyncWebServerRequest* req) {
    bool newBand2gOnly = req->hasParam("band") && req->getParam("band")->value() == "2g";
    preferences.putBool("band2gOnly", newBand2gOnly);
    req->send(200, "text/plain", "Band preference saved! Rebooting device now...");
    delay(1000);
    ESP.restart();
  });

  server.on("/savenetwork", HTTP_GET, [](AsyncWebServerRequest* req) {
    bool enableStatic = req->hasParam("enabled") && req->getParam("enabled")->value() == "1";
    preferences.putBool("staticIpOn", enableStatic);
    if (enableStatic) {
      preferences.putString("staticIp", req->hasParam("ip") ? req->getParam("ip")->value() : "");
      preferences.putString("staticSubnet", req->hasParam("subnet") ? req->getParam("subnet")->value() : "");
      preferences.putString("staticGw", req->hasParam("gateway") ? req->getParam("gateway")->value() : "");
      preferences.putString("staticDns1", req->hasParam("dns1") ? req->getParam("dns1")->value() : "");
      preferences.putString("staticDns2", req->hasParam("dns2") ? req->getParam("dns2")->value() : "");
    }
    req->send(200, "text/plain", "Network settings saved! Rebooting device now...");
    delay(1000);
    ESP.restart();
  });

  server.on("/savemesh", HTTP_GET, [](AsyncWebServerRequest* req) {
    bool enableMesh = req->hasParam("enabled") && req->getParam("enabled")->value() == "1";
    preferences.putBool("meshOn", enableMesh);
    if (enableMesh) {
      preferences.putString("meshNodeId", req->hasParam("nodeid") ? req->getParam("nodeid")->value() : "");
      preferences.putString("meshName", req->hasParam("meshname") ? req->getParam("meshname")->value() : "");
      preferences.putString("meshPass", req->hasParam("meshpass") ? req->getParam("meshpass")->value() : "");
      preferences.putString("meshServer", req->hasParam("server") ? req->getParam("server")->value() : "");
    }
    req->send(200, "text/plain", "Mesh settings saved! Rebooting device now...");
    delay(1000);
    ESP.restart();
  });

  server.on("/meshconfig", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"enabled\":";
    json += meshModeEnabled ? "true" : "false";
    json += ",\"nodeid\":\"" + meshNodeId + "\"";
    json += ",\"meshname\":\"" + meshName + "\"";
    json += ",\"server\":\"" + meshServerAddr + "\"}";
    // Mesh password is deliberately left out of this readback.
    req->send(200, "application/json", json);
  });

  server.on("/networkconfig", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"enabled\":";
    json += useStaticIp ? "true" : "false";
    json += ",\"ip\":\"" + staticIp + "\"";
    json += ",\"subnet\":\"" + staticSubnet + "\"";
    json += ",\"gateway\":\"" + staticGateway + "\"";
    json += ",\"dns1\":\"" + staticDns1 + "\"";
    json += ",\"dns2\":\"" + staticDns2 + "\"}";
    req->send(200, "application/json", json);
  });

  server.on("/ridscanstart", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!ridScanActive) {
      ridScanActive = true;
      ridDroneCount = 0;
      if (req->hasParam("mode") && req->getParam("mode")->value() == "wifi") {
        if (pcapCaptureActive) {
          ridScanActive = false;  // roll back -- refusing to start
          req->send(409, "text/plain", "Can't start WiFi-mode RID scan while a packet capture is running.");
          return;
        }
        ridScanMode = 1;
        xTaskCreate(wifiRidScanTask, "wifiRidScanTask", 4096, NULL, 1, &ridScanTaskHandle);
      } else {
        ridScanMode = 0;
        xTaskCreate(ridScanTask, "ridScanTask", 4096, NULL, 1, &ridScanTaskHandle);
      }
    }
    req->send(200, "application/json", "{\"running\":true}");
  });

  server.on("/ridscanstop", HTTP_GET, [](AsyncWebServerRequest* req) {
    ridScanActive = false;
    req->send(200, "application/json", "{\"running\":false}");
  });

  server.on("/ridscanstatus", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"running\":" + String(ridScanActive ? "true" : "false") + ",\"drones\":" + ridBuildJson() + "}";
    req->send(200, "application/json", json);
  });

  server.on("/pcapstart", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (pcapCaptureActive) {
      req->send(409, "text/plain", "A packet capture is already running.");
      return;
    }
    if (ridScanActive) {
      req->send(409, "text/plain", "Can't start packet capture while a WiFi-mode RID scan is running.");
      return;
    }
    bool mgmtOnly = req->hasParam("mgmtonly") && req->getParam("mgmtonly")->value() == "1";
    if (!startPcapCaptureIfIdle(mgmtOnly)) {
      req->send(500, "text/plain", "Failed to open capture file on SPIFFS.");
      return;
    }
    req->send(200, "text/plain", String("Packet capture started (") + (pcapMgmtOnly ? "management frames only" : "all 802.11 traffic") + ").");
  });

  server.on("/pcapstop", HTTP_GET, [](AsyncWebServerRequest* req) {
    stopPcapCapture();
    req->send(200, "text/plain", "Packet capture stopping.");
  });

  server.on("/pcapstatus", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"active\":";
    json += pcapCaptureActive ? "true" : "false";
    json += ",\"mgmtOnly\":";
    json += pcapMgmtOnly ? "true" : "false";
    json += ",\"packetCount\":" + String(pcapPacketCount);
    json += ",\"bytesWritten\":" + String(pcapBytesWritten);
    json += ",\"spiffsFree\":" + String(SPIFFS.totalBytes() - SPIFFS.usedBytes());
    json += "}";
    req->send(200, "application/json", json);
  });

  server.on("/pcapdownload", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!SPIFFS.exists(PCAP_FILE_PATH)) {
      req->send(404, "text/plain", "No capture file found. Run a capture first.");
      return;
    }
    req->send(SPIFFS, PCAP_FILE_PATH, "application/vnd.tcpdump.pcap", true);
  });

  server.on("/blescan", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (bleScanTaskRunning) {
      req->send(409, "text/plain", "A BLE scan is already running.");
      return;
    }
    bleScanTaskRunning = true;
    bleScanTaskResult = "Scanning for BLE devices...";
    bleScanDevicesJson = "[]";
    xTaskCreate(bleScanTask, "bleScanTask", 8192, NULL, 1, NULL);
    req->send(200, "text/plain", "BLE scan started.");
  });

  server.on("/blescanstatus", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"running\":";
    json += bleScanTaskRunning ? "true" : "false";
    json += ",\"result\":\"" + jsonEscapeForWifiScan(bleScanTaskResult) + "\"";
    json += ",\"devices\":" + bleScanDevicesJson + "}";
    req->send(200, "application/json", json);
  });

  server.on("/radarstart", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("mac")) {
      req->send(400, "text/plain", "Missing mac parameter.");
      return;
    }
    radarTargetMac = req->getParam("mac")->value();
    radarLastRssi = -127;
    radarLastSeenMs = 0;
    if (!radarActive) {
      radarActive = true;
      xTaskCreate(radarTask, "radarTask", 8192, NULL, 1, NULL);
    }
    req->send(200, "text/plain", "Tracking started.");
  });

  server.on("/radarstop", HTTP_GET, [](AsyncWebServerRequest* req) {
    radarActive = false;
    req->send(200, "text/plain", "Tracking stopped.");
  });

  server.on("/radarstatus", HTTP_GET, [](AsyncWebServerRequest* req) {
    unsigned long agoMs = (radarLastSeenMs == 0) ? 999999 : (millis() - radarLastSeenMs);
    bool found = radarActive && agoMs < 3000;
    String json = "{\"active\":";
    json += radarActive ? "true" : "false";
    json += ",\"found\":";
    json += found ? "true" : "false";
    json += ",\"rssi\":" + String(radarLastRssi);
    json += ",\"mac\":\"" + radarTargetMac + "\"}";
    req->send(200, "application/json", json);
  });

  server.on("/traceroute", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("target")) {
      req->send(400, "text/plain", "Missing target parameter.");
      return;
    }
    if (traceTaskRunning) {
      req->send(409, "text/plain", "A traceroute is already running.");
      return;
    }
    traceTargetInput = req->getParam("target")->value();
    traceMaxHopsInput = 30;
    traceTimeoutInput = 2000;
    if (req->hasParam("maxhops")) {
      int mh = req->getParam("maxhops")->value().toInt();
      if (mh >= 1 && mh <= 30) traceMaxHopsInput = mh;
    }
    if (req->hasParam("timeout")) {
      int to = req->getParam("timeout")->value().toInt();
      if (to >= 200 && to <= 5000) traceTimeoutInput = to;
    }
    traceTaskRunning = true;
    traceTaskResult = "Starting traceroute...";
    xTaskCreate(traceTask, "traceTask", 6144, NULL, 1, NULL);
    req->send(200, "text/plain", "Traceroute started.");
  });

  server.on("/traceroutestatus", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"running\":";
    json += traceTaskRunning ? "true" : "false";
    json += ",\"result\":\"" + jsonEscapeForWifiScan(traceTaskResult) + "\"}";
    req->send(200, "application/json", json);
  });

  server.on("/dnslookup", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("target")) {
      req->send(400, "text/plain", "Missing target parameter.");
      return;
    }
    if (dnsLookupRunning) {
      req->send(409, "text/plain", "A DNS lookup is already running.");
      return;
    }
    dnsLookupInput = req->getParam("target")->value();
    dnsLookupRunning = true;
    dnsLookupResult = "Looking up...";
    xTaskCreate(dnsLookupTask, "dnsLookupTask", 6144, NULL, 1, NULL);
    req->send(200, "text/plain", "DNS lookup started.");
  });

  server.on("/dnslookupstatus", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"running\":";
    json += dnsLookupRunning ? "true" : "false";
    json += ",\"result\":\"" + jsonEscapeForWifiScan(dnsLookupResult) + "\"}";
    req->send(200, "application/json", json);
  });

  server.on("/whois", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("target")) {
      req->send(400, "text/plain", "Missing target parameter.");
      return;
    }
    if (whoisTaskRunning) {
      req->send(409, "text/plain", "A WHOIS lookup is already running.");
      return;
    }
    whoisInput = req->getParam("target")->value();
    whoisTaskRunning = true;
    whoisTaskResult = "Running WHOIS...";
    xTaskCreate(whoisTask, "whoisTask", 6144, NULL, 1, NULL);
    req->send(200, "text/plain", "WHOIS started.");
  });

  server.on("/whoisstatus", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"running\":";
    json += whoisTaskRunning ? "true" : "false";
    json += ",\"result\":\"" + jsonEscapeForWifiScan(whoisTaskResult) + "\"}";
    req->send(200, "application/json", json);
  });

  server.on("/reversedns", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("target")) {
      req->send(400, "text/plain", "Missing target parameter.");
      return;
    }
    if (reverseDnsRunning) {
      req->send(409, "text/plain", "A reverse DNS lookup is already running.");
      return;
    }
    reverseDnsInput = req->getParam("target")->value();
    reverseDnsRunning = true;
    reverseDnsResult = "Looking up...";
    xTaskCreate(reverseDnsTask, "reverseDnsTask", 6144, NULL, 1, NULL);
    req->send(200, "text/plain", "Reverse DNS lookup started.");
  });

  server.on("/reversednsstatus", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"running\":";
    json += reverseDnsRunning ? "true" : "false";
    json += ",\"result\":\"" + jsonEscapeForWifiScan(reverseDnsResult) + "\"}";
    req->send(200, "application/json", json);
  });

  server.on("/heap", HTTP_GET, [](AsyncWebServerRequest* req) {
    String text = "Free heap: " + String(ESP.getFreeHeap()) + " bytes\n";
    text += "Min free heap ever: " + String(ESP.getMinFreeHeap()) + " bytes\n";
    text += "Largest free block: " + String(ESP.getMaxAllocHeap()) + " bytes\n";
    req->send(200, "text/plain", text);
  });

  server.on("/portscan", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (portScanRunning) {
      req->send(409, "text/plain", "A port scan is already running.");
      return;
    }
    if (!req->hasParam("target")) {
      req->send(400, "text/plain", "missing target param");
      return;
    }
    portScanTargetInput = req->getParam("target")->value();
    portScanSingleMode = req->hasParam("mode") && req->getParam("mode")->value() == "single";
    portScanSinglePort = req->hasParam("port") ? req->getParam("port")->value().toInt() : 0;
    portScanSingleIsUdp = req->hasParam("proto") && req->getParam("proto")->value() == "udp";
    if (portScanSingleMode && (portScanSinglePort <= 0 || portScanSinglePort > 65535)) {
      req->send(400, "text/plain", "invalid port");
      return;
    }
    portScanRunning = true;
    portScanResult = "Starting port scan...";
    xTaskCreate(portScanTask, "portScanTask", 8192, NULL, 1, NULL);
    req->send(200, "text/plain", "Port scan started.");
  });

  server.on("/portscanstatus", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"running\":";
    json += portScanRunning ? "true" : "false";
    json += ",\"result\":\"" + jsonEscapeForWifiScan(portScanResult) + "\"}";
    req->send(200, "application/json", json);
  });

  server.on("/arpsweep", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (arpSweepRunning) {
      req->send(409, "text/plain", "A subnet sweep is already running.");
      return;
    }
    arpSweepRunning = true;
    arpSweepResult = "Starting subnet sweep...";
    xTaskCreate(arpSweepTask, "arpSweepTask", 8192, NULL, 1, NULL);
    req->send(200, "text/plain", "Subnet sweep started.");
  });

  server.on("/arpsweepstatus", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"running\":";
    json += arpSweepRunning ? "true" : "false";
    json += ",\"result\":\"" + jsonEscapeForWifiScan(arpSweepResult) + "\"}";
    req->send(200, "application/json", json);
  });

  server.on("/portsweep", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (portSweepRunning) {
      req->send(409, "text/plain", "A port sweep is already running.");
      return;
    }
    if (!req->hasParam("port")) {
      req->send(400, "text/plain", "missing port param");
      return;
    }
    portSweepPort = req->getParam("port")->value().toInt();
    portSweepIsUdp = req->hasParam("proto") && req->getParam("proto")->value() == "udp";
    if (portSweepPort <= 0 || portSweepPort > 65535) {
      req->send(400, "text/plain", "invalid port");
      return;
    }
    portSweepRunning = true;
    portSweepResult = "Starting port sweep...";
    xTaskCreate(portSweepTask, "portSweepTask", 8192, NULL, 1, NULL);
    req->send(200, "text/plain", "Port sweep started.");
  });

  server.on("/portsweepstatus", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"running\":";
    json += portSweepRunning ? "true" : "false";
    json += ",\"result\":\"" + jsonEscapeForWifiScan(portSweepResult) + "\"}";
    req->send(200, "application/json", json);
  });

  server.on("/sysstatus", HTTP_GET, [](AsyncWebServerRequest* req) {
    float chipTemp = 0;
    bool haveTemp = false;
    if (tempSensorOk && temperature_sensor_get_celsius(tempSensorHandle, &chipTemp) == ESP_OK) {
      haveTemp = true;
    }
    String json = "{";
    json += "\"uptimeSec\":" + String(millis() / 1000) + ",";
    json += "\"cpuFreqMHz\":" + String(ESP.getCpuFreqMHz()) + ",";
    json += "\"loopHz\":" + String(lastLoopHz, 1) + ",";
    json += "\"heapFree\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"heapTotal\":" + String(ESP.getHeapSize()) + ",";
    json += "\"heapMinFree\":" + String(ESP.getMinFreeHeap()) + ",";
    json += "\"heapLargestBlock\":" + String(ESP.getMaxAllocHeap()) + ",";
    json += "\"sketchUsed\":" + String(ESP.getSketchSize()) + ",";
    json += "\"sketchFree\":" + String(ESP.getFreeSketchSpace()) + ",";
    json += "\"flashChipSize\":" + String(ESP.getFlashChipSize()) + ",";
    json += "\"chipTempC\":" + (haveTemp ? String(chipTemp, 1) : String("null")) + ",";
    json += "\"wifiRssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"wifiChannel\":" + String(WiFi.channel()) + ",";
    json += "\"wifiIp\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"tasks\":[" + sysStatusTaskList() + "]";
    json += "}";
    req->send(200, "application/json", json);
  });

  server.on("/tftpupload", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (tftpUploadTooBig) {
        request->send(507, "text/plain", "Not enough storage space available.");
      } else if (tftpUploadError.length() > 0) {
        request->send(500, "text/plain", tftpUploadError);
      } else {
        request->send(200, "text/plain", "Upload complete.");
      }
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      String safeName = tftpSanitizeFilename(filename);
      if (index == 0) {
        tftpUploadReceived = 0;
        tftpUploadTooBig = false;
        tftpUploadError = "";
        size_t freeNow = SPIFFS.totalBytes() - SPIFFS.usedBytes();
        tftpUploadCap = (freeNow > TFTP_FREE_MARGIN) ? (freeNow - TFTP_FREE_MARGIN) : 0;
        tftpUploadFile = SPIFFS.open(safeName, "w");
        if (!tftpUploadFile) {
          tftpUploadError = "Could not open file for writing.";
        }
      }
      if (tftpUploadFile && !tftpUploadTooBig) {
        if (tftpUploadReceived + len > tftpUploadCap) {
          tftpUploadTooBig = true;
          tftpUploadFile.close();
        } else {
          tftpUploadFile.write(data, len);
          tftpUploadReceived += len;
        }
      }
      if (final) {
        if (tftpUploadFile) tftpUploadFile.close();
        if (tftpUploadTooBig) {
          SPIFFS.remove(safeName);
        }
      }
    }
  );

  server.on("/tftpfiles", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"usedBytes\":" + String(SPIFFS.usedBytes()) + ",\"totalBytes\":" + String(SPIFFS.totalBytes()) + ",\"files\":[";
    File root = SPIFFS.open("/");
    File f = root.openNextFile();
    bool first = true;
    while (f) {
      if (!first) json += ",";
      first = false;
      json += "{\"name\":\"" + tftpJsonEscape(String(f.name())) + "\",\"size\":" + String(f.size()) + "}";
      f = root.openNextFile();
    }
    json += "]}";
    req->send(200, "application/json", json);
  });

  server.on("/tftpdelete", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("name", true)) {
      req->send(400, "text/plain", "Missing name parameter.");
      return;
    }
    String name = tftpSanitizeFilename(req->getParam("name", true)->value());
    if (SPIFFS.exists(name)) {
      SPIFFS.remove(name);
      req->send(200, "text/plain", "Deleted.");
    } else {
      req->send(404, "text/plain", "File not found.");
    }
  });

  server.on("/tftpdaemonstart", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!tftpDaemonActive) {
      tftpDaemonActive = true;
      xTaskCreate(tftpDaemonTask, "tftpDaemon", 8192, NULL, 1, &tftpDaemonTaskHandle);
    }
    req->send(200, "application/json", "{\"running\":true}");
  });

  server.on("/tftpdaemonstop", HTTP_GET, [](AsyncWebServerRequest* req) {
    tftpDaemonActive = false;
    req->send(200, "application/json", "{\"running\":false}");
  });

  server.on("/tftpdaemonstatus", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", String("{\"running\":") + (tftpDaemonActive ? "true" : "false") + "}");
  });

  server.on("/netstatus", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", doNetStatus());
  });

  server.on("/led", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("on")) {
      rgbLedOn = req->getParam("on")->value().toInt() != 0;
      rememberLedPref = true;
      preferences.putBool("rememberLed", true);
      preferences.putBool("ledOff", !rgbLedOn);
      updateStatusLed();
    }
    req->send(200, "text/plain", rgbLedOn ? "on" : "off");
  });

  server.on("/wifiscan", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (wifiScanTaskRunning) {
      req->send(200, "text/plain", "A scan is already running.");
      return;
    }
    bool want5g = req->hasParam("band") && req->getParam("band")->value() == "5g";
    wifiScanRequestedBand = want5g ? 1 : 0;
    wifiScanTaskRunning = true;
    wifiScanTaskResult = want5g ? "Scanning 5GHz..." : "Scanning 2.4GHz...";
    xTaskCreate(wifiScanTask, "wifiScanTask", 8192, NULL, 1, NULL);
    req->send(200, "text/plain", "Scan started.");
  });

  server.on("/wifiscanstatus", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"running\":";
    json += wifiScanTaskRunning ? "true" : "false";
    json += ",\"result\":\"" + jsonEscapeForWifiScan(wifiScanTaskResult) + "\"}";
    req->send(200, "application/json", json);
  });

  server.on("/ping", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (pingTaskRunning) {
      req->send(409, "text/plain", "A ping is already running.");
      return;
    }
    if (!req->hasParam("target") || req->getParam("target")->value().length() == 0) {
      req->send(400, "text/plain", "Missing target.");
      return;
    }
    pingTargetInput = req->getParam("target")->value();
    pingTtlInput = req->hasParam("ttl") ? req->getParam("ttl")->value().toInt() : 64;
    pingSizeInput = req->hasParam("size") ? req->getParam("size")->value().toInt() : 64;
    if (pingTtlInput < 1 || pingTtlInput > 255) pingTtlInput = 64;
    if (pingSizeInput < 0 || pingSizeInput > 1400) pingSizeInput = 64;
    pingContinuousMode = req->hasParam("continuous") && req->getParam("continuous")->value() == "1";
    pingStopRequested = false;
    pingTaskResult = "Starting...";
    pingTaskRunning = true;
    xTaskCreate(pingTask, "pingTask", 4096, NULL, 1, NULL);
    req->send(200, "text/plain", "started");
  });

  server.on("/pingstop", HTTP_GET, [](AsyncWebServerRequest* req) {
    pingStopRequested = true;
    req->send(200, "text/plain", "stopping");
  });

  server.on("/pingstatus", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"running\":";
    json += pingTaskRunning ? "true" : "false";
    json += ",\"result\":\"" + jsonEscapeForWifiScan(pingTaskResult) + "\"";
    json += ",\"count\":" + String(pingReceivedCount);
    json += ",\"min\":" + String(pingReceivedCount > 0 ? pingMinMs : 0);
    json += ",\"max\":" + String(pingMaxMs);
    float avgMsNow = (pingReceivedCount > 0) ? ((float)pingSumMs / (float)pingReceivedCount) : 0.0f;
    json += ",\"avg\":" + String(avgMsNow, 1);
    json += ",\"last\":" + String(pingLastElapsedMs);
    json += "}";
    req->send(200, "application/json", json);
  });

  server.onNotFound([](AsyncWebServerRequest* req) {
    if (isApMode) {
      AsyncWebServerResponse* response = req->beginResponse(302, "text/plain", "");
      response->addHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
      req->send(response);
    } else {
      req->send(404, "text/plain", "not found");
    }
  });

  server.begin();
  Serial.println("Web server started");
}

String meshStatusTopic() { return "fortigill/" + meshName + "/" + meshNodeId + "/status"; }
String meshCmdTopic() { return "fortigill/" + meshName + "/" + meshNodeId + "/cmd"; }
String meshTelemetryTopic() { return "fortigill/" + meshName + "/" + meshNodeId + "/telemetry"; }
String meshVersionTopic() { return "fortigill/" + meshName + "/" + meshNodeId + "/version"; }

// Minimal flat-JSON string-field extractor -- looks for "key":"value" and
// returns value. Good enough for our simple {"action":"...","tool":"..."}
// command payloads without pulling in a full JSON library.
String meshJsonField(const String &json, const String &key) {
  String keyPattern = "\"" + key + "\"";
  int idx = json.indexOf(keyPattern);
  if (idx < 0) return "";
  int pos = idx + keyPattern.length();
  // Tolerate whitespace around the colon (e.g. Python's json.dumps emits
  // "key": "value" with a space, not "key":"value").
  while (pos < (int)json.length() && (json[pos] == ' ' || json[pos] == ':')) pos++;
  if (pos >= (int)json.length() || json[pos] != '"') return "";
  pos++;  // skip the value's opening quote
  int end = json.indexOf("\"", pos);
  if (end < 0) return "";
  return json.substring(pos, end);
}

long meshJsonFieldInt(const String &json, const String &key) {
  String keyPattern = "\"" + key + "\"";
  int idx = json.indexOf(keyPattern);
  if (idx < 0) return -1;
  int pos = idx + keyPattern.length();
  while (pos < (int)json.length() && (json[pos] == ' ' || json[pos] == ':')) pos++;
  int start = pos;
  while (pos < (int)json.length() && isDigit(json[pos])) pos++;
  if (pos == start) return -1;
  return json.substring(start, pos).toInt();
}

void startBleScanIfIdle() {
  if (!bleScanTaskRunning) {
    bleScanTaskRunning = true;
    bleScanTaskResult = "Scanning for BLE devices...";
    bleScanDevicesJson = "[]";
    xTaskCreate(bleScanTask, "bleScanTask", 8192, NULL, 1, NULL);
  }
}

void startRidScanIfIdle() {
  if (!ridScanActive) {
    ridScanActive = true;
    ridDroneCount = 0;
    ridScanMode = 0;  // remote-triggered / scheduled starts default to BLE mode
    xTaskCreate(ridScanTask, "ridScanTask", 4096, NULL, 1, &ridScanTaskHandle);
  }
}

// Called every loop() tick. Re-triggers a scan the moment its schedule's
// interval has elapsed, using the exact same start path as a manual
// start, so scheduled runs behave identically and still publish over
// mesh via the existing meshPublishTelemetryIfDue() change-detection.
void meshRunScheduledScansIfDue() {
  if (!meshModeEnabled || !meshServerConnected) return;
  unsigned long now = millis();
  if (bleScheduleActive && (long)(now - bleScheduleNextRunMs) >= 0) {
    startBleScanIfIdle();
    bleScheduleNextRunMs = now + bleScheduleIntervalMs;
  }
  if (ridScheduleActive && (long)(now - ridScheduleNextRunMs) >= 0) {
    startRidScanIfIdle();
    ridScheduleNextRunMs = now + ridScheduleIntervalMs;
  }
}

void meshMqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  String action = meshJsonField(msg, "action");
  String tool = meshJsonField(msg, "tool");
  Serial.println("Mesh: received command: " + action + " " + tool);

  if (action == "send_pcap") {
    xTaskCreate(pcapUploadTask, "pcapUploadTask", 8192, NULL, 1, NULL);
    return;
  }

  if (action == "pcap_start") {
    long mgmtOnly = meshJsonFieldInt(msg, "mgmt_only");
    bool started = startPcapCaptureIfIdle(mgmtOnly != 0);
    Serial.println(started ? "Mesh: packet capture started"
                            : "Mesh: packet capture start request ignored (already running or busy)");
    return;
  }

  if (action == "pcap_stop") {
    stopPcapCapture();
    Serial.println("Mesh: packet capture stop requested");
    return;
  }

  if (tool == "ble_scan") {
    if (action == "start") {
      startBleScanIfIdle();
    } else if (action == "stop") {
      bleScanTaskRunning = false;
    } else if (action == "schedule") {
      long minutes = meshJsonFieldInt(msg, "interval_minutes");
      if (minutes > 0) {
        bleScheduleActive = true;
        bleScheduleIntervalMs = (unsigned long)minutes * 60000UL;
        bleScheduleNextRunMs = millis();  // run immediately, then repeat
        Serial.println("Mesh: BLE scan scheduled every " + String(minutes) + " min");
      }
    } else if (action == "unschedule") {
      bleScheduleActive = false;
      Serial.println("Mesh: BLE scan schedule cleared");
    }
  } else if (tool == "rid_scan") {
    if (action == "start") {
      startRidScanIfIdle();
    } else if (action == "stop") {
      ridScanActive = false;
    } else if (action == "schedule") {
      long minutes = meshJsonFieldInt(msg, "interval_minutes");
      if (minutes > 0) {
        ridScheduleActive = true;
        ridScheduleIntervalMs = (unsigned long)minutes * 60000UL;
        ridScheduleNextRunMs = millis();
        Serial.println("Mesh: RID scan scheduled every " + String(minutes) + " min");
      }
    } else if (action == "unschedule") {
      ridScheduleActive = false;
      Serial.println("Mesh: RID scan schedule cleared");
    }
  }
  // "radar" intentionally not handled here -- see the note at the top of
  // this patch script.
}

void meshMqttReconnect() {
  if (!meshModeEnabled || meshName.length() == 0 || meshServerAddr.length() == 0) {
    meshServerConnected = false;
    return;
  }
  if (meshMqttClient.connected()) {
    meshServerConnected = true;
    return;
  }
  meshServerConnected = false;

  unsigned long now = millis();
  if (now - meshLastReconnectAttemptMs < 5000) return;
  meshLastReconnectAttemptMs = now;

  meshMqttClient.setServer(meshServerAddr.c_str(), 1883);
  meshMqttClient.setCallback(meshMqttCallback);
  meshMqttClient.setBufferSize(2048);  // the #define alone doesn't reach PubSubClient.cpp's own build -- this does

  String clientId = meshNodeId.length() > 0 ? meshNodeId : ("esp32c5-" + WiFi.macAddress());
  String statusTopic = meshStatusTopic();

  bool ok = meshMqttClient.connect(clientId.c_str(), meshName.c_str(), meshPassword.c_str(),
                                    statusTopic.c_str(), 1, true, "offline");
  if (ok) {
    meshServerConnected = true;
    meshMqttClient.publish(statusTopic.c_str(), "online", true);
    meshMqttClient.publish(meshVersionTopic().c_str(), FIRMWARE_VERSION, true);
    meshMqttClient.subscribe(meshCmdTopic().c_str());
    Serial.println("Mesh: connected to MQTT server as " + clientId);
  } else {
    Serial.println("Mesh: MQTT connect failed, rc=" + String(meshMqttClient.state()));
  }
}

void meshPublishTelemetryIfDue() {
  static unsigned long lastMeshTelemetryMs = 0;
  static String lastPublishedRidJson = "";
  static String lastPublishedBleJson = "";
  if (!meshModeEnabled || !meshServerConnected) return;
  unsigned long now = millis();
  if (now - lastMeshTelemetryMs < 5000) return;
  lastMeshTelemetryMs = now;

  // Check every tick (so a freshly-finished scan's real result is
  // caught promptly -- bleScanTask() flips its running flag back to
  // false at essentially the same moment it populates final results,
  // so gating publish on that flag reliably missed it), but only
  // actually publish when the content changed, so an unchanged result
  // doesn't get republished forever once a scan finishes.
  String ridJsonNow = ridBuildJson();
  if (ridJsonNow != lastPublishedRidJson) {
    meshMqttClient.publish(meshTelemetryTopic().c_str(), ridJsonNow.c_str());
    lastPublishedRidJson = ridJsonNow;
  }
  if (bleScanDevicesJson != lastPublishedBleJson) {
    meshMqttClient.publish(meshTelemetryTopic().c_str(), bleScanDevicesJson.c_str());
    lastPublishedBleJson = bleScanDevicesJson;
  }
}

void loop() {
  loopCounter++;
  {
    unsigned long nowMs = millis();
    if (nowMs - lastLoopRateCalcMs >= 1000) {
      lastLoopHz = (float)loopCounter * 1000.0 / (float)(nowMs - lastLoopRateCalcMs);
      loopCounter = 0;
      lastLoopRateCalcMs = nowMs;
    }
  }

  if (isApMode) dnsServer.processNextRequest();
  updateStatusLed();

  if (!isApMode) {
    meshMqttReconnect();
    meshMqttClient.loop();
    meshPublishTelemetryIfDue();
    meshRunScheduledScansIfDue();
  }

  if (connectedVia5g && !fellBackTo2g && !isApMode) {
    if (WiFi.status() == WL_CONNECTED) {
      lastConnectedMs = millis();
    } else if (millis() - lastConnectedMs > BAND_FALLBACK_TIMEOUT_MS) {
      Serial.println("Lost the 5GHz connection for too long - falling back to 2.4GHz.");
      fellBackTo2g = true;
      WiFi.setAutoReconnect(false);
      WiFi.disconnect();
      WiFi.setBandMode(WIFI_BAND_MODE_2G_ONLY);
      delay(150);
      int n2g = WiFi.scanNetworks(false, false, false, 300, 0, savedSsid.c_str());
      if (n2g > 0) {
        wifi_ap_record_t *rec = (wifi_ap_record_t *)WiFi.getScanInfoByIndex(0);
        Serial.println("Found on 2.4GHz, RSSI " + String(rec->rssi) + " dBm - connecting there.");
        WiFi.begin(savedSsid.c_str(), savedPass.c_str(), rec->primary, rec->bssid);
      } else {
        Serial.println("Didn't see the saved network on 2.4GHz either - trying a normal connect anyway.");
        WiFi.begin(savedSsid.c_str(), savedPass.c_str());
      }
      WiFi.scanDelete();
      WiFi.setAutoReconnect(true);
      lastConnectedMs = millis();
    }
  }

  delay(10);
}
