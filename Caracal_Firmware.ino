/*
   ESP8266 WiFi Shield for xatLabs IBIS converter
   (C) 2017-2018 Julian Metzler
*/

/*
   IMPORTANT

   For the HTTPS OTA update to work, the verify() method of TLSTraits
   in ESP8266HTTPClient.cpp needs to be modified to always return true.
*/

/*
   UPLOAD SETTINGS

   Board: Generic ESP8266 Module
   Flash Mode: DIO
   Flash Size: 4M
   SPIFFS Size: 1M
   Debug port: Disabled
   Debug Level: None
   Reset Method: ck
   Flash Freq: 40 MHz
   CPU Freq: 80 MHz
   Upload Speed: 115200
*/

#define ARDUINO_OTA_ENABLEDXXX
#define INIT_EEPROMXXX
#define SERIAL_DEBUGXXX

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <FS.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266mDNS.h>

#ifdef ARDUINO_OTA_ENABLED
#include <ArduinoOTA.h>
#endif

#include "ibis.h"

/*
   TYPEDEFS
*/

enum UpdateStatus {
  US_NO_UPDATE,
  US_AVAILABLE,
  US_FAILED
};

/*
   CONSTANTS
*/

#define HW_GROUP 2 // Changes with hardware changes that require software changes

#if HW_GROUP == 1
#define PIN_STATUS    10
#define PIN_ACTIVITY   9 // Unusable
#define PIN_CONFIG     0
#endif

#if HW_GROUP == 2
#define PIN_STATUS    5
#define PIN_ACTIVITY  4
#define PIN_CONFIG    0
#endif

#define WIFI_TIMEOUT 10000
#define UPDATE_START_DELAY 3000
#define EEPROM_SIZE 128

/*
   GLOBAL VARIABLES
*/

unsigned long FW_VERSION = 1906300001;    // Changes with each release; must always increase
unsigned long SP_VERSION = 0;             // Loaded from SPIFFS; changed with each SPIFFS build; must always increase (uses timestamp as version)

// FW & SPIFFS update settings
const char* UPDATE_HOST = "static.mezgrman.de";
const int UPDATE_PORT_HTTPS = 443;
const int UPDATE_PORT_HTTP = 80;
String UPDATE_PATH_BASE_HTTPS = "/firmware/wifi_shield/";
String UPDATE_URL_BASE_HTTPS = "https://static.mezgrman.de/firmware/wifi_shield/";
String UPDATE_URL_BASE_HTTP = "http://static.mezgrman.de/firmware/wifi_shield/";
String UPDATE_FINGERPRINT_HTTPS = "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"; // Not important, will be ignored anyway

// Update flags
bool updateFWSecure_flag = false;
bool updateSPSecure_flag = false;
bool updateFWInsecure_flag = false;
bool updateSPInsecure_flag = false;
unsigned long updateFlagSetTimestamp = 0;

// Start time of the last WiFi connection attempt
unsigned long wifiTimer = 0;
bool wifiTimedOut = false;

// Variables for Station WiFi
String STA_SSID;
String STA_PASS;
String STA_HOSTNAME;
char STA_HOSTNAME_CHAR[33];
bool STA_SETUP = false;

// Variables for Access Point WiFi
String AP_SSID = "xatLabs WiFi Module";
String AP_PASS = "xatlabs_wifi";
bool AP_ACTIVE = false;

// Variables for keeping track of the config button
volatile bool btnState = 0;           // Current button state
bool oldBtnState = 0;                 // Previous button state
volatile bool btnPressed = 0;         // Flag to check if the last button press has already been processed
volatile unsigned long btnTimer = 0;  // Start time of last button press (only while pressed)
volatile unsigned long btnDur = 0;    // Duration of the last button press (only while released)

ESP8266WebServer server(80);
WiFiServer IBISServer(5001);
WiFiClient client;

/*
   CONFIG FILE HANDLING
*/

void reset_EEPROM() {
  for (byte i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0x00);
  }
  EEPROM.commit();
}

void reset_EEPROM(uint16_t startAddress, uint16_t endAddress) {
  for (uint16_t i = startAddress; (i < EEPROM_SIZE && i < endAddress); i++) {
    EEPROM.write(i, 0x00);
  }
  EEPROM.commit();
}

bool loadConfig() {
  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile) {
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    return false;
  }

  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);

  // We don't use String here because ArduinoJson library requires the input
  // buffer to be mutable. If you don't use ArduinoJson, you may as well
  // use configFile.readString instead.
  configFile.readBytes(buf.get(), size);

  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(buf.get());

  if (!json.success()) {
    return false;
  }

  SP_VERSION = json["SPVersion"];

  char curChar;
  bool ssidSetup = false;
  bool passSetup = false;
  bool hostnameSetup = false;
  STA_SSID = "";
  for (byte i = 0; i < 32; i++) {
    curChar = EEPROM.read(i);
    if (curChar == 0x00) {
      break;
    }
    STA_SSID += curChar;
    ssidSetup = true;
  }
  STA_PASS = "";
  for (byte i = 0; i < 64; i++) {
    curChar = EEPROM.read(i + 32);
    if (curChar == 0x00) {
      break;
    }
    STA_PASS += curChar;
    passSetup = true;
  }
  STA_HOSTNAME = "";
  for (byte i = 0; i < 32; i++) {
    curChar = EEPROM.read(i + 96);
    if (curChar == 0x00) {
      break;
    }
    STA_HOSTNAME += curChar;
    hostnameSetup = true;
  }
  if (STA_HOSTNAME == "") STA_HOSTNAME = "xatLabs-WiFi-Module";
  STA_HOSTNAME.toCharArray(STA_HOSTNAME_CHAR, 32);
  STA_SETUP = (ssidSetup && passSetup);

  return true;
}

bool saveConfig() {
  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  json["SPVersion"] = SP_VERSION;

  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    return false;
  }
  json.printTo(configFile);

  reset_EEPROM();
  for (byte i = 0; i < STA_SSID.length(); i++) {
    EEPROM.write(i, STA_SSID.charAt(i));
  }
  for (byte i = 0; i < STA_PASS.length(); i++) {
    EEPROM.write(i + 32, STA_PASS.charAt(i));
  }
  for (byte i = 0; i < STA_HOSTNAME.length(); i++) {
    EEPROM.write(i + 96, STA_HOSTNAME.charAt(i));
  }
  EEPROM.commit();

  return true;
}

/*
   HELPER FUNCTIONS
*/

void setLEDStatus(bool state) {
  digitalWrite(PIN_STATUS, state);
}

void blinkLEDStatusSingle(unsigned int duration) {
  setLEDStatus(1);
  delay(duration);
  setLEDStatus(0);
}

void blinkLEDStatusLoop(unsigned int duration) {
  blinkLEDStatusSingle(duration);
  delay(duration);
}

void setLEDActivity(bool state) {
#if HW_GROUP == 1
  digitalWrite(PIN_STATUS, state);
#else
  digitalWrite(PIN_ACTIVITY, state);
#endif
}

void blinkLEDActivitySingle(unsigned int duration) {
  setLEDActivity(1);
  delay(duration);
  setLEDActivity(0);
}

void blinkLEDActivityLoop(unsigned int duration) {
  blinkLEDActivitySingle(duration);
  delay(duration);
}

void handleConfigButton() {
  // Read the config button state to determine if it has been pressed or released
  btnState = !digitalRead(PIN_CONFIG);
  // Calculate the last press duration
  if (btnState) {
    btnTimer = millis();
    btnDur = 0;
  } else {
    btnDur = millis() - btnTimer;
    btnTimer = 0;
    // Discard presses <= 50ms
    if (btnDur > 50) {
      btnPressed = 1;
    } else {
      btnDur = 0;
    }
  }
}

/*
   WEB SERVER
*/

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

String formatPageBaseWithExtraHead(String content, String extraHead) {
  String page;
  page += "<html>";
  page += "<head>";
  page += "<link rel='shortcut icon' href='/favicon.ico'>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  page += "<meta charset='UTF-8'>";
  page += "<link rel='stylesheet' href='/main.css'>";
  page += "<title>WiFi Module</title>";
  page += extraHead;
  page += "</head>";
  page += "<body>";
  page += "<img id='logo' src='/xatlabs_logo.png'>";
  page += content;
  page += "</body>";
  page += "</html>";
  return page;
}

String formatPageBase(String content) {
  return formatPageBaseWithExtraHead(content, "");
}

void handleRoot() {
  String c;
  c += "<h1>WiFi Module</h1>";
  c += "<h2>WiFi Setup</h2>";
  c += "<div>Current SSID: <b>";
  if (AP_ACTIVE) {
    c += AP_SSID;
  } else if (STA_SETUP) {
    c += STA_SSID;
  } else {
    c += "None";
  }
  c += "</b></div>";
  c += "<form action='/wifi-setup' method='post'>";
  c += "<table>";
  c += "<tr><td>SSID</td><td><input type='text' name='ssid' maxlength='32' /></td></tr>";
  c += "<tr><td>Password</td><td><input type='password' name='password' maxlength='64' /></td></tr>";
  c += "<tr><td><input type='submit' value='Save and Reboot' /></td></tr>";
  c += "</table>";
  c += "</form>";
  c += "<h2>Hostname Setup</h2>";
  c += "<div>Current Hostname: <b>";
  c += STA_HOSTNAME;
  c += "</b></div>";
  c += "<div>Attention! Valid characters: A-Z, a-z, 0-9 and -</div>";
  c += "<div>You can use this to access the WiFi Module at <a href='http://";
  c += STA_HOSTNAME;
  c += ".local/'>http://";
  c += STA_HOSTNAME;
  c += ".local/</a></div>";
  c += "<form action='/hostname-setup' method='post'>";
  c += "<table>";
  c += "<tr><td>Hostname</td><td><input type='text' name='hostname' maxlength='32' /></td></tr>";
  c += "<tr><td><input type='submit' value='Save and Reboot' /></td></tr>";
  c += "</table>";
  c += "</form>";
  c += "<a href='/check-update'>Check for firmware update</a>";
  server.send(200, "text/html", formatPageBase(c));
}

void handle_wifi_setup() {
  STA_SSID = server.arg("ssid");
  STA_PASS = server.arg("password");
  STA_SETUP = true;
  saveConfig();
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
  ESP.restart();
}

void handle_hostname_setup() {
  STA_HOSTNAME = server.arg("hostname");
  STA_HOSTNAME.toCharArray(STA_HOSTNAME_CHAR, 32);
  saveConfig();
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
  ESP.restart();
}

void handle_check_update() {
  String c;
  UpdateStatus fwStatusSecure = checkForUpdateSecure(false);
  UpdateStatus spStatusSecure = checkForUpdateSecure(true);
  UpdateStatus fwStatusInsecure = checkForUpdateInsecure(false);
  UpdateStatus spStatusInsecure = checkForUpdateInsecure(true);
  c += "<h1>Update Status</h1>";
  c += "<table><tr><td>Firmware version:</td>";
  c += "<td>";
  c += "WS-";
  c += HW_GROUP;
  c += "-";
  c += FW_VERSION;
  c += "</td>";
  /*if (fwStatusSecure == US_AVAILABLE) {
    c += "<td>Update available (HTTPS)</td>";
    c += "<td><form action='/update-fw-https' method='post'><input type='submit' value='Update (HTTPS)' /></form></td>";
    } else if (fwStatusSecure == US_FAILED) {
    c += "<td>Update check failed (HTTPS)</td>";
    } else if (fwStatusSecure == US_NO_UPDATE) {
    c += "<td>No update available (HTTPS)</td>";
    }*/
  if (fwStatusInsecure == US_AVAILABLE) {
    c += "<td>Update available (HTTP)</td>";
    c += "<td><form action='/update-fw-http' method='post'><input type='submit' value='Update (HTTP)' /></form></td>";
  } else if (fwStatusInsecure == US_FAILED) {
    c += "<td>Update check failed (HTTP)</td>";
  } else if (fwStatusInsecure == US_NO_UPDATE) {
    c += "<td>No update available (HTTP)</td>";
  }
  c += "</tr>";
  c += "<tr><td>Filesystem version:</td>";
  c += "<td>";
  c += "WSF-";
  c += HW_GROUP;
  c += "-";
  c += SP_VERSION;
  c += "</td>";
  /*if (spStatusSecure == US_AVAILABLE) {
    c += "<td>Update available (HTTPS)</td>";
    c += "<td><form action='/update-sp-https' method='post'><input type='submit' value='Update (HTTPS)' /></form></td>";
    } else if (spStatusSecure == US_FAILED) {
    c += "<td>Update check failed (HTTPS)</td>";
    } else if (spStatusSecure == US_NO_UPDATE) {
    c += "<td>No update available (HTTPS)</td>";
    }*/
  if (spStatusInsecure == US_AVAILABLE) {
    c += "<td>Update available (HTTP)</td>";
    c += "<td><form action='/update-sp-http' method='post'><input type='submit' value='Update (HTTP)' /></form></td>";
  } else if (spStatusInsecure == US_FAILED) {
    c += "<td>Update check failed (HTTP)</td>";
  } else if (spStatusInsecure == US_NO_UPDATE) {
    c += "<td>No update available (HTTP)</td>";
  }
  c += "</tr></table>";
  server.send(200, "text/html", formatPageBase(c));
}

void handle_update_fw_https() {
  updateFWSecure_flag = true;
  updateFlagSetTimestamp = millis();
  server.sendHeader("Location", "/update-running", true);
  server.send(303, "text/plain", "");
}

void handle_update_sp_https() {
  updateSPSecure_flag = true;
  updateFlagSetTimestamp = millis();
  server.sendHeader("Location", "/update-running", true);
  server.send(303, "text/plain", "");
}

void handle_update_fw_http() {
  updateFWInsecure_flag = true;
  updateFlagSetTimestamp = millis();
  server.sendHeader("Location", "/update-running", true);
  server.send(303, "text/plain", "");
}

void handle_update_sp_http() {
  updateSPInsecure_flag = true;
  updateFlagSetTimestamp = millis();
  server.sendHeader("Location", "/update-running", true);
  server.send(303, "text/plain", "");
}

void handle_update_running() {
  String c;
  c += "<h1>Update in progress...</h1>";
  c += "<p>Please wait while the update is being downloaded and installed.</p>";
  server.send(200, "text/html", formatPageBaseWithExtraHead(c, "<meta http-equiv='refresh' content='5'>"));
}

/*
   INTERRUPT ROUTINES
*/

void ISR_config() {
  handleConfigButton();
}

/*
   PROGRAM ROUTINES
*/

void doWiFiConfigViaWPS() {
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.beginWPSConfig();
  while (WiFi.status() != WL_CONNECTED) {
    blinkLEDStatusLoop(250);
  }
  STA_SSID = WiFi.SSID();
  STA_PASS = WiFi.psk();
  STA_SETUP = true;
  saveConfig();
  for (int i = 0; i < 3; i++) {
    blinkLEDStatusLoop(125);
  }
}

void doWiFiConfigViaAP() {
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID.c_str(), AP_PASS.c_str());
  AP_ACTIVE = true;
  setLEDStatus(1);
}

void printIPAddress() {
  // TODO: Stops working after the first time
  IPAddress addr = WiFi.localIP();
  String addrStr;
  addrStr += String(addr[0]);
  addrStr += ".";
  addrStr += String(addr[1]);
  addrStr += ".";
  addrStr += String(addr[2]);
  addrStr += ".";
  addrStr += String(addr[3]);
  IBIS_DS009(addrStr);
  IBIS_DS003c(addrStr);
  IBIS_GSP(1, "WLAN-Modul", addrStr);
}

void resetWiFiCredentials() {
  STA_SSID = "";
  STA_PASS = "";
  STA_HOSTNAME = "xatLabs-WiFi-Module";
  STA_SETUP = false;
  saveConfig();
  ESP.restart();
}

/*
   FIRMWARE & SPIFFS UPDATE
*/

UpdateStatus checkForUpdateSecure(bool spiffs) {
  String url = UPDATE_PATH_BASE_HTTPS + HW_GROUP;
  if (spiffs) {
    url += "/spiffs.version";
  } else {
    url += "/firmware.version";
  }
  WiFiClientSecure httpsClient;
  if (!httpsClient.connect(UPDATE_HOST, UPDATE_PORT_HTTPS)) return US_FAILED;
  httpsClient.println("GET " + url + " HTTP/1.0");
  httpsClient.print("Host: ");
  httpsClient.println(UPDATE_HOST);
  httpsClient.println("Connection: close");
  httpsClient.println();
  while (httpsClient.connected()) {
    String line = httpsClient.readStringUntil('\n');
    if (line == "\r") {
      // Headers received
      break;
    }
  }
  String newVersionStr = httpsClient.readStringUntil('\n');
  unsigned long newVersion = newVersionStr.toInt();
  if (spiffs) {
    if (newVersion > SP_VERSION ) {
      return US_AVAILABLE;
    }
  } else {
    if (newVersion > FW_VERSION ) {
      return US_AVAILABLE;
    }
  }
  return US_NO_UPDATE;
}

UpdateStatus checkForUpdateInsecure(bool spiffs) {
  String url = UPDATE_URL_BASE_HTTP + HW_GROUP;
  if (spiffs) {
    url += "/spiffs.version";
  } else {
    url += "/firmware.version";
  }
  HTTPClient httpClient;
  httpClient.begin(url);
  int httpCode = httpClient.GET();
  if (httpCode == 200) {
    String newVersionStr = httpClient.getString();
    unsigned long newVersion = newVersionStr.toInt();
    if (spiffs) {
      if (newVersion > SP_VERSION ) {
        return US_AVAILABLE;
      }
    } else {
      if (newVersion > FW_VERSION ) {
        return US_AVAILABLE;
      }
    }
  } else {
    return US_FAILED;
  }
  return US_NO_UPDATE;
}

UpdateStatus checkForUpdate(bool spiffs) {
  // First check securely, fallback to insecure
  UpdateStatus statusSecure = US_FAILED;//checkForUpdateSecure(spiffs);
  if (statusSecure == US_FAILED) {
    return checkForUpdateInsecure(spiffs);
  } else {
    return statusSecure;
  }
}

UpdateStatus checkForFWUpdate() {
  return checkForUpdate(false);
}

UpdateStatus checkForSPUpdate() {
  return checkForUpdate(true);
}

t_httpUpdate_return doUpdateSecure(bool spiffs) {
  // Set both LEDs on during update
  setLEDStatus(1);
  pinMode(2, OUTPUT);
  digitalWrite(2, LOW);

  t_httpUpdate_return ret;
  String url = UPDATE_URL_BASE_HTTPS + HW_GROUP;
  if (spiffs) {
    url += "/spiffs.bin";
    ret = ESPhttpUpdate.updateSpiffs(url, "", UPDATE_FINGERPRINT_HTTPS);
  } else {
    url += "/firmware.bin";
    ret = ESPhttpUpdate.update(url, "", UPDATE_FINGERPRINT_HTTPS);
  }
  if (ret == HTTP_UPDATE_OK) {
    ESP.restart();
  }
  return ret;
}

t_httpUpdate_return doUpdateInsecure(bool spiffs) {
  // Set both LEDs on during update
  setLEDStatus(1);
  pinMode(2, OUTPUT);
  digitalWrite(2, LOW);

  t_httpUpdate_return ret;
  String url = UPDATE_URL_BASE_HTTP + HW_GROUP;
  if (spiffs) {
    url += "/spiffs.bin";
    ret = ESPhttpUpdate.updateSpiffs(url, "");
  } else {
    url += "/firmware.bin";
    ret = ESPhttpUpdate.update(url, "");
  }
  if (ret == HTTP_UPDATE_OK) {
    ESP.restart();
  }
  return ret;
}

void doUpdate(bool spiffs) {
  // Set both LEDs on during update
  setLEDStatus(1);
  setLEDActivity(1);
  pinMode(2, OUTPUT);
  digitalWrite(2, LOW);

  t_httpUpdate_return ret;
  // Try secure update first
  ret = HTTP_UPDATE_FAILED;//doUpdateSecure(spiffs);

  if (ret == HTTP_UPDATE_FAILED) {
    // Failover to insecure update
    ret = doUpdateInsecure(spiffs);

    if (ret == HTTP_UPDATE_FAILED) {
      setLEDStatus(0);
      digitalWrite(2, LOW);
      pinMode(2, INPUT);
      for (int i = 0; i < 3; i++) {
        blinkLEDStatusLoop(750);
      }
    }
  }
}

void doFWUpdate() {
  doUpdate(false);
}

void doSPUpdate() {
  doUpdate(true);
}

/*
   MAIN PROGRAM
*/

void setup() {
  WiFi.mode(WIFI_AP);

  pinMode(PIN_STATUS, OUTPUT);
#if HW_GROUP != 1
  pinMode(PIN_ACTIVITY, OUTPUT);
#endif
  pinMode(PIN_CONFIG, INPUT_PULLUP);
  //attachInterrupt(digitalPinToInterrupt(PIN_CONFIG), ISR_config, CHANGE);

  EEPROM.begin(EEPROM_SIZE); // 32 for SSID, 64 for PSK, 32 for hostname

  // Reset hostname area of EEPROM if required
  if (EEPROM.read(96) == 0xff) {
    reset_EEPROM(96, 128);
  }

#ifdef INIT_EEPROM
  reset_EEPROM();
#endif

  SPIFFS.begin();

  loadConfig();

#ifdef ARDUINO_OTA_ENABLED
  ArduinoOTA.setHostname(STA_HOSTNAME);
  ArduinoOTA.begin();
#endif

  if (STA_SETUP) {
    // WiFi connection has been set up
    WiFi.mode(WIFI_STA);
    WiFi.begin(STA_SSID.c_str(), STA_PASS.c_str());
    wifiTimer = millis();
    while (WiFi.status() != WL_CONNECTED) {
      if ((millis() - wifiTimer) > WIFI_TIMEOUT) {
        wifiTimedOut = true;
        break;
      }
      blinkLEDStatusLoop(250);
    }
    if (wifiTimedOut) {
      // Connection timed out, fall back to AP mode
      doWiFiConfigViaAP();
    } else {
      // Connected
      WiFi.hostname(STA_HOSTNAME);
    }
  } else {
    // No WiFi connection has yet been set up, enter AP mode
    doWiFiConfigViaAP();
  }

  // Set up time
  configTime(1 * 3600, 0, "pool.ntp.org");

  // Set up mDNS responder
  MDNS.begin(STA_HOSTNAME_CHAR);

  IBISServer.begin();

  server.onNotFound(handleNotFound);
  server.on("/", handleRoot);
  server.on("/wifi-setup", handle_wifi_setup);
  server.on("/hostname-setup", handle_hostname_setup);
  server.on("/check-update", handle_check_update);
  server.on("/update-fw-https", handle_update_fw_https);
  server.on("/update-sp-https", handle_update_sp_https);
  server.on("/update-fw-http", handle_update_fw_http);
  server.on("/update-sp-http", handle_update_sp_http);
  server.on("/update-running", handle_update_running);
  server.serveStatic("/main.css", SPIFFS, "/main.css");
  server.serveStatic("/favicon.ico", SPIFFS, "/favicon.ico");
  server.serveStatic("/xatlabs_logo.png", SPIFFS, "/xatlabs_logo.png");
  server.begin();

#ifdef SERIAL_DEBUG
  Serial.begin(115200);
  Serial.setDebugOutput(1);
#else
  IBIS_init();
#endif

  for (int i = 0; i < 3; i++) {
    blinkLEDStatusLoop(60);
    blinkLEDActivityLoop(60);
  }

  if (AP_ACTIVE) {
    setLEDStatus(1);
  } else {
    if (checkForFWUpdate() == US_AVAILABLE) {
      doFWUpdate();
    }
    if (checkForSPUpdate() == US_AVAILABLE) {
      doSPUpdate();
    }
  }
}

void loop() {
#ifdef ARDUINO_OTA_ENABLED
  ArduinoOTA.handle();
#endif
  server.handleClient();

  btnState = !digitalRead(PIN_CONFIG);
  if (btnState != oldBtnState) {
    oldBtnState = btnState;
    handleConfigButton();
  }

  WiFiClient newClient = IBISServer.available();
  if (newClient) client = newClient;
  if (client) {
    while (client.available()) {
      setLEDActivity(1);
      Serial.write(client.read());
      setLEDActivity(0);
    }
    while (Serial.available()) {
      setLEDActivity(1);
      client.write(Serial.read());
      setLEDActivity(0);
    }
  }

  // Handle LED blinking while button is being pressed (visual timing help)
  if (btnState) {
    unsigned long dur = (millis() - btnTimer) % 1000;
    setLEDStatus(dur > 500);
  }

  // Update a certain time after flag is set
  // (to give the web server time to send the redirect after initiating the update)
  if (updateFWSecure_flag) {
    if (millis() - updateFlagSetTimestamp >= UPDATE_START_DELAY) {
      updateFWSecure_flag = false;
      updateFlagSetTimestamp = 0;
      if (checkForUpdateSecure(false) == US_AVAILABLE) {
        doUpdateSecure(false);
      }
    }
  }
  if (updateSPSecure_flag) {
    if (millis() - updateFlagSetTimestamp >= UPDATE_START_DELAY) {
      updateSPSecure_flag = false;
      updateFlagSetTimestamp = 0;
      if (checkForUpdateSecure(true) == US_AVAILABLE) {
        doUpdateSecure(true);
      }
    }
  }
  if (updateFWInsecure_flag) {
    if (millis() - updateFlagSetTimestamp >= UPDATE_START_DELAY) {
      updateFWInsecure_flag = false;
      updateFlagSetTimestamp = 0;
      if (checkForUpdateInsecure(false) == US_AVAILABLE) {
        doUpdateInsecure(false);
      }
    }
  }
  if (updateSPInsecure_flag) {
    if (millis() - updateFlagSetTimestamp >= UPDATE_START_DELAY) {
      updateSPInsecure_flag = false;
      updateFlagSetTimestamp = 0;
      if (checkForUpdateInsecure(true) == US_AVAILABLE) {
        doUpdateInsecure(true);
      }
    }
  }

  // Check if the button has been pressed and for how long
  if (btnPressed) {
    btnPressed = 0;
    setLEDStatus(0);
    int selectedOption = btnDur / 1000;
    switch (selectedOption) {
      case 0: {
          // Short press
          break;
        }
      case 1: {
          // WPS mode
          doWiFiConfigViaWPS();
          break;
        }
      case 2: {
          // AP mode
          doWiFiConfigViaAP();
          break;
        }
      case 3: {
          // AP mode
          printIPAddress();
          break;
        }
      case 10: {
          // Reset WiFi Credentials
          resetWiFiCredentials();
          break;
        }
      default: {
          break;
        }
    }
  }
}

