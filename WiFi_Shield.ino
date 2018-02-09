/*
   ESP8266 WiFi Shield for xatLabs IBIS converter
   (C) 2017-2018 Julian Metzler
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

#include <ESP8266WiFi.h>
//#include <WiFiClientSecure.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <FS.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>

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

#define PIN_STATUS 10
#define PIN_CONFIG 0

#define WIFI_TIMEOUT 10000

/*
   GLOBAL VARIABLES
*/

unsigned long HW_GROUP = 1;               // Changes with hardware changes that require software changes
unsigned long FW_VERSION = 1802080001;    // Changes with each release; must always increase
unsigned long SP_VERSION = 0;             // Loaded from SPIFFS; changed with each SPIFFS build; must always increase (uses timestamp as version)

// HTTPS update settings
//String UPDATE_HOST = "static.mezgrman.de";
//int UPDATE_PORT = 443;

// HTTP update settings
String UPDATE_URL_BASE = "http://static.mezgrman.de/firmware/wifi_shield/";

// Start time of the last WiFi connection attempt
unsigned long wifiTimer = 0;
bool wifiTimedOut = false;

// Variables for Station WiFi
String STA_SSID;
String STA_PASS;
bool STA_SETUP = false;

// Variables for Access Point WiFi
String AP_SSID = "xatLabs WiFi Shield";
String AP_PASS = "xatlabs_wifi";
bool AP_ACTIVE = false;

// Variables for keeping track of the config button
volatile bool btnState = 0;           // Current button state
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
  for (byte i = 0; i < 96; i++) {
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

  /*STA_SSID = json["WiFiSSID"].as<String>();
    STA_PASS = json["WiFiPassword"].as<String>();
    STA_SETUP = json["WiFiSetup"];*/
  SP_VERSION = json["SPVersion"];

  char curChar;
  bool ssidSetup = false;
  bool passSetup = false;
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
  STA_SETUP = (ssidSetup && passSetup);

  return true;
}

bool saveConfig() {
  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  /*json["WiFiSSID"] = STA_SSID;
    json["WiFiPassword"] = STA_PASS;
    json["WiFiSetup"] = STA_SETUP;*/
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
  EEPROM.commit();

  return true;
}

/*
   HELPER FUNCTIONS
*/

void setLED(bool state) {
  digitalWrite(PIN_STATUS, state);
}

void blinkLEDSingle(unsigned int duration) {
  setLED(1);
  delay(duration);
  setLED(0);
}

void blinkLEDLoop(unsigned int duration) {
  blinkLEDSingle(duration);
  delay(duration);
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

String formatPageBase(String content) {
  String page;
  page += "<html>";
  page += "<head>";
  page += "<link rel='shortcut icon' href='/favicon.ico'>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  page += "<meta charset='UTF-8'>";
  page += "<link rel='stylesheet' href='/main.css'>";
  page += "<title>WiFi Shield</title>";
  page += "</head>";
  page += "<body>";
  page += content;
  page += "</body>";
  page += "</html>";
  return page;
}

void handleRoot() {
  String c;
  c += "<h1>WiFi Shield</h1>";
  c += "<div>Current SSID: ";
  if (AP_ACTIVE) {
    c += AP_SSID;
  } else if (STA_SETUP) {
    c += STA_SSID;
  } else {
    c += "None";
  }
  c += "</div>";
  c += "<form action='/wifi-setup' method='post'>";
  c += "<table>";
  c += "<tr><td>SSID</td><td><input type='text' name='ssid' /></td></tr>";
  c += "<tr><td>Password</td><td><input type='password' name='password' /></td></tr>";
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

void handle_check_update() {
  String c;
  UpdateStatus fwStatus = checkForFWUpdate();
  UpdateStatus spStatus = checkForSPUpdate();
  c += "<h1>Update Status</h1>";
  c += "<table><tr><td>Firmware version:</td>";
  c += "<td>";
  c += "WS-";
  c += HW_GROUP;
  c += "-";
  c += FW_VERSION;
  c += "</td>";
  if (fwStatus == US_AVAILABLE) {
    c += "<td>Update available</td>";
    c += "<td><form action='/update-fw' method='post'><input type='submit' value='Update' /></form></td>";
  } else if (fwStatus == US_FAILED) {
    c += "<td>Update check failed</td>";
  } else if (fwStatus == US_NO_UPDATE) {
    c += "<td>No update available</td>";
  }
  c += "</tr>";
  c += "<tr><td>Filesystem version:</td>";
  c += "<td>";
  c += "WSF-";
  c += HW_GROUP;
  c += "-";
  c += SP_VERSION;
  c += "</td>";
  if (spStatus == US_AVAILABLE) {
    c += "<td>Update available</td>";
    c += "<td><form action='/update-sp' method='post'><input type='submit' value='Update' /></form></td>";
  } else if (spStatus == US_FAILED) {
    c += "<td>Update check failed</td>";
  } else if (spStatus == US_NO_UPDATE) {
    c += "<td>No update available</td>";
  }
  c += "</tr></table>";
  server.send(200, "text/html", formatPageBase(c));
}

void handle_update_fw() {
  server.sendHeader("Location", "/check-update", true);
  server.send(303, "text/plain", "");
  if (checkForFWUpdate() == US_AVAILABLE) {
    doFWUpdate();
  }
}

void handle_update_sp() {
  server.sendHeader("Location", "/check-update", true);
  server.send(303, "text/plain", "");
  if (checkForSPUpdate() == US_AVAILABLE) {
    doSPUpdate();
  }
}

/*
   INTERRUPT ROUTINES
*/

void ISR_config() {
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
   MAIN PROGRAM ROUTINES
*/

void doWiFiConfigViaWPS() {
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.beginWPSConfig();
  while (WiFi.status() != WL_CONNECTED) {
    blinkLEDLoop(250);
  }
  STA_SSID = WiFi.SSID();
  STA_PASS = WiFi.psk();
  STA_SETUP = true;
  saveConfig();
  for (int i = 0; i < 3; i++) {
    blinkLEDLoop(125);
  }
}

void doWiFiConfigViaAP() {
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID.c_str(), AP_PASS.c_str());
  AP_ACTIVE = true;
  setLED(1);
}

UpdateStatus checkForFWUpdate() {
  /*WiFiClientSecure httpsClient;
    if (!httpsClient.connect(UPDATE_HOST.c_str(), UPDATE_PORT)) {
    //Serial.print("connection failed");
    }
    httpsClient.println("GET /firmware/wifi_shield/1/firmware.version HTTP/1.1");
    httpsClient.println("Host: " + UPDATE_HOST);
    httpsClient.println("Connection: close");
    httpsClient.println();
    delay(1000);
    while (httpsClient.available()) {
    String line = httpsClient.readStringUntil('\n');
    Serial.print(line);
    }
    return 2;*/
  String url = UPDATE_URL_BASE + HW_GROUP + "/firmware.version";
  HTTPClient httpClient;
  httpClient.begin(url);
  int httpCode = httpClient.GET();
  if (httpCode == 200) {
    String newFWVersion = httpClient.getString();
    unsigned long newVersion = newFWVersion.toInt();
    if (newVersion > FW_VERSION ) {
      return US_AVAILABLE;
    }
  } else {
    return US_FAILED;
  }
  return US_NO_UPDATE;
}

void doFWUpdate() {
  // Set both LEDs on during update
  setLED(1);
  pinMode(2, OUTPUT);
  digitalWrite(2, LOW);
  String url = UPDATE_URL_BASE + HW_GROUP + "/firmware.bin";
  t_httpUpdate_return ret = ESPhttpUpdate.update(url);

}

UpdateStatus checkForSPUpdate() {
  String url = UPDATE_URL_BASE + HW_GROUP + "/spiffs.version";
  HTTPClient httpClient;
  httpClient.begin(url);
  int httpCode = httpClient.GET();
  if (httpCode == 200) {
    String newSPVersion = httpClient.getString();
    unsigned long newVersion = newSPVersion.toInt();
    if (newVersion > SP_VERSION ) {
      return US_AVAILABLE;
    }
  } else {
    return US_FAILED;
  }
  return US_NO_UPDATE;
}

void doSPUpdate() {
  // Set both LEDs on during update
  setLED(1);
  pinMode(2, OUTPUT);
  digitalWrite(2, LOW);
  String url = UPDATE_URL_BASE + HW_GROUP + "/spiffs.bin";
  t_httpUpdate_return ret = ESPhttpUpdate.updateSpiffs(url);
}

void setup() {
  pinMode(PIN_STATUS, OUTPUT);
  pinMode(PIN_CONFIG, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_CONFIG), ISR_config, CHANGE);

#ifdef ARDUINO_OTA_ENABLED
  ArduinoOTA.setHostname("WiFi-Shield");
  ArduinoOTA.begin();
#endif

  EEPROM.begin(96); // 96 bytes - 32 for SSID, 64 for PSK

#ifdef INIT_EEPROM
  reset_EEPROM();
#endif

  SPIFFS.begin();

  loadConfig();

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
      blinkLEDLoop(250);
    }
    if (wifiTimedOut) {
      // Connection timed out, fall back to AP mode
      doWiFiConfigViaAP();
    }
  } else {
    // No WiFi connection has yet been set up, enter AP mode
    doWiFiConfigViaAP();
  }

  IBISServer.begin();

  server.onNotFound(handleNotFound);
  server.on("/", handleRoot);
  server.on("/wifi-setup", handle_wifi_setup);
  server.on("/check-update", handle_check_update);
  server.on("/update-fw", handle_update_fw);
  server.on("/update-sp", handle_update_sp);
  server.serveStatic("/main.css", SPIFFS, "/main.css");
  server.serveStatic("/favicon.ico", SPIFFS, "/favicon.ico");
  server.begin();

  IBIS_init();

  for (int i = 0; i < 3; i++) {
    blinkLEDLoop(125);
  }

  if (AP_ACTIVE) {
    setLED(1);
  } else {
    if (checkForFWUpdate()) {
      doFWUpdate();
    }
    if (checkForSPUpdate()) {
      doSPUpdate();
    }
  }
}

void loop() {
#ifdef ARDUINO_OTA_ENABLED
  ArduinoOTA.handle();
#endif
  server.handleClient();

  WiFiClient newClient = IBISServer.available();
  if (newClient) client = newClient;
  if (client) {
    while (client.available()) {
      setLED(1);
      Serial.write(client.read());
      setLED(0);
    }
  }

  // Handle LED blinking while button is being pressed (visual timing help)
  if (btnState) {
    unsigned long dur = (millis() - btnTimer) % 1000;
    setLED(dur > 500);
  }

  // Check if the button has been pressed and for how long
  if (btnPressed) {
    btnPressed = 0;
    setLED(0);
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
      default: {
          break;
        }
    }
  }
}

