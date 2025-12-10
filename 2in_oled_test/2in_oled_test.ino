#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <string.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <ESP8266HTTPClient.h>

// === DISPLAY PINS (your original wiring) ===
#define TFT_CS   D8     // CS
#define TFT_DC   D2     // DC
#define TFT_RST  D0     // RESET
// SCLK = D5, MOSI = D7 (hardware SPI)

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// === CONFIG AP SETTINGS ===
const char* AP_SSID     = "TeleDraw-Setup";
const char* AP_PASSWORD = "12345678";

// === SERVER INFO ===
String serverHost;
uint16_t serverPort = 3000;
const char* SERVER_PATH = "/";
unsigned long lastWifiCheck = 0;

ESP8266WebServer configServer(80);
WebSocketsClient webSocket;

String wifiSsid;
String wifiPass;

int tftLogLine = 0;

void tftLog(const char* msg) {
  const int16_t x = 0;
  const int16_t startY = 10;
  const int16_t lineH = 10;

  int16_t y = startY + tftLogLine * lineH;

  // Simple scroll: clear and start over if we run off the bottom
  if (y > 240 - lineH) {
    tft.fillScreen(ST77XX_BLACK);
    tftLogLine = 0;
    y = startY;
  }

  tft.setCursor(x, y);
  tft.println(msg);
  tftLogLine++;
}

// ---------- CONFIG HANDLING ----------

bool loadConfig() {
  Serial.println("[CFG] loadConfig()");

  if (!LittleFS.exists("/config.json")) {
    Serial.println("[CFG] /config.json does not exist");
    return false;
  }

  File f = LittleFS.open("/config.json", "r");
  if (!f) {
    Serial.println("[CFG] Failed to open /config.json");
    return false;
  }

  String content = f.readString();
  f.close();

  Serial.println("[CFG] Raw file contents:");
  Serial.println(content);

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, content);
  if (err) {
    Serial.print("[CFG] JSON parse error: ");
    Serial.println(err.c_str());
    return false;
  }

  if (!doc.containsKey("ssid") ||
      !doc.containsKey("pass") ||
      !doc.containsKey("server") ||
      !doc.containsKey("port")) {
    Serial.println("[CFG] JSON missing keys");
    return false;
  }

  wifiSsid   = doc["ssid"].as<String>();
  wifiPass   = doc["pass"].as<String>();
  serverHost = doc["server"].as<String>();
  serverPort = doc["port"].as<uint16_t>();

  Serial.print("[CFG] SSID: ");
  Serial.println(wifiSsid);
  Serial.print("[CFG] PASS length: ");
  Serial.println(wifiPass.length());
  Serial.print("[CFG] SERVER: ");
  Serial.println(serverHost);
  Serial.print("[CFG] PORT: ");
  Serial.println(serverPort);

  if (wifiSsid.length() == 0 || serverHost.length() == 0 || serverPort == 0) {
    Serial.println("[CFG] Invalid SSID/server/port");
    return false;
  }

  Serial.println("[CFG] Config OK");
  return true;
}

bool saveConfig() {
  Serial.println("[CFG] saveConfig()");

  StaticJsonDocument<256> doc;
  doc["ssid"]   = wifiSsid;
  doc["pass"]   = wifiPass;
  doc["server"] = serverHost;
  doc["port"]   = serverPort;

  File f = LittleFS.open("/config.json", "w");
  if (!f) {
    Serial.println("[CFG] Failed to open /config.json for writing");
    return false;
  }

  if (serializeJson(doc, f) == 0) {
    Serial.println("[CFG] Failed to write JSON");
    f.close();
    return false;
  }

  f.close();
  Serial.println("[CFG] Config saved");
  return true;
}

// ---------- CONFIG PORTAL ----------

void startConfigPortal() {
  Serial.println("[AP] Starting config portal");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress ip = WiFi.softAPIP();
  Serial.print("[AP] AP IP: ");
  Serial.println(ip);

  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(10, 10);
  tft.println("AP setup mode");
  tft.setCursor(10, 30);
  tft.println(ip.toString());

  configServer.on("/", []() {
    String html = R"(
      <html><body>
      <h1>TeleDraw Setup</h1>
      <form method='POST' action='/save'>
  
        WiFi SSID:<br>
        <input name='ssid'><br><br>
  
        WiFi Password:<br>
        <input name='pass' type='password'><br><br>
  
        Server IP / Host:<br>
        <input name='server' placeholder="192.168.0.221"><br><br>
  
        Server Port:<br>
        <input name='port' placeholder="3000"><br><br>
  
        <input type='submit' value='Save & Reboot'>
      </form>
      </body></html>
    )";
    configServer.send(200, "text/html", html);
  });

  configServer.on("/save", HTTP_POST, []() {
    Serial.println("[AP] /save called");
  
    wifiSsid   = configServer.arg("ssid");
    wifiPass   = configServer.arg("pass");
    serverHost = configServer.arg("server");
  
    String portStr = configServer.arg("port");
    serverPort = portStr.toInt();
  
    Serial.print("[AP] New SSID: ");
    Serial.println(wifiSsid);
    Serial.print("[AP] New PASS length: ");
    Serial.println(wifiPass.length());
    Serial.print("[AP] New SERVER: ");
    Serial.println(serverHost);
    Serial.print("[AP] New PORT: ");
    Serial.println(serverPort);
  
    if (serverPort == 0) {
      serverPort = 3000;  // safety fallback
      Serial.println("[AP] Invalid port entered, defaulting to 3000");
    }
  
    saveConfig();
  
    configServer.send(200, "text/plain", "Saved! Rebooting...");
    delay(1000);
    ESP.restart();
  });


  configServer.begin();
  Serial.println("[AP] Config server started");

  // Block forever in portal
  while (true) {
    configServer.handleClient();
    delay(10);
  }
}

// ---------- WIFI ----------

bool connectWiFi() {
  Serial.println("[WiFi] connectWiFi()");
  Serial.print("[WiFi] SSID: ");
  Serial.println(wifiSsid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 25000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Failed to connect");
    return false;
  }

  Serial.print("[WiFi] Connected! IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

// ---------- WEBSOCKET ----------

uint16_t parseHexColor565(const char* hex) {
  if (!hex) return ST77XX_WHITE;

  // Skip leading '#'
  if (hex[0] == '#') {
    hex++;
  }

  // Expect exactly 6 hex digits now
  if (strlen(hex) != 6) {
    return ST77XX_WHITE;
  }

  auto hexToByte = [](char high, char low) -> uint8_t {
    auto nibble = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
      if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
      return 0;
    };
    return (nibble(high) << 4) | nibble(low);
  };

  uint8_t r = hexToByte(hex[0], hex[1]);
  uint8_t g = hexToByte(hex[2], hex[3]);
  uint8_t b = hexToByte(hex[4], hex[5]);

  return tft.color565(r, g, b);
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[WS] Connected");
      {
        StaticJsonDocument<64> doc;
        doc["type"] = "hello";
        doc["role"] = "device";
        String out;
        serializeJson(doc, out);
        webSocket.sendTXT(out);
      }
      break;

    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected");
      break;

    case WStype_TEXT: {
      StaticJsonDocument<256> doc;
      if (deserializeJson(doc, payload, len)) {
        Serial.println("[WS] JSON parse error");
        return;
      }

      const char* typeStr = doc["type"] | "";
      static uint32_t strokeCounter = 0;
      if (strcmp(typeStr, "stroke") == 0) {
        int x1 = doc["from"][0] | 0;
        int y1 = doc["from"][1] | 0;
        int x2 = doc["to"][0] | 0;
        int y2 = doc["to"][1] | 0;
        const char* colorStr = doc["color"] | "#ffffff";
        uint16_t color = parseHexColor565(colorStr);
      
        int width = doc["width"] | 2;   // default if not sent
        if (width < 1) width = 1;
        int radius = (width - 1) / 2;   // 2→0, 4→1, 6→2, 8→3, 10→4
      
        // basic bounds check
        if (x1 < 0 || x1 >= 320 || x2 < 0 || x2 >= 320 ||
            y1 < 0 || y1 >= 240 || y2 < 0 || y2 >= 240) {
          return;
        }
      
        if (radius == 0) {
          // smallest brush, just a single line
          tft.drawLine(x1, y1, x2, y2, color);
        } else {
          // thicker brush: stamp circles along the line
          int dx = x2 - x1;
          int dy = y2 - y1;
          int steps = max(abs(dx), abs(dy));
          if (steps == 0) {
            // single point
            tft.fillCircle(x1, y1, radius, color);
          } else {
            for (int i = 0; i <= steps; i++) {
              int x = x1 + (dx * i) / steps;
              int y = y1 + (dy * i) / steps;
      
              if (x >= 0 && x < 320 && y >= 0 && y < 240) {
                tft.fillCircle(x, y, radius, color);
              }
      
              // let WiFi/watchdog breathe occasionally
              if ((i & 0x3F) == 0) { // every 64 steps
                yield();
              }
            }
          }
        }
      
        strokeCounter++;
        if (strokeCounter % 200 == 0) {
          yield();
        }
      } else if (strcmp(typeStr, "fill") == 0) {
        const char* colorStr = doc["color"] | "#000000";
        uint16_t color = parseHexColor565(colorStr);
        tft.fillScreen(color);
      }
      break;
    }

    default:
      break;
  }
}


void setupWS() {
  Serial.printf("[WS] Connecting to %s:%u%s\n",
                serverHost.c_str(), serverPort, SERVER_PATH);

  webSocket.begin(serverHost.c_str(), serverPort, SERVER_PATH);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);

  // Keep-alive so it survives overnight
  webSocket.enableHeartbeat(60000, 10000, 3);
}


// ---------- TFT ----------

void setupTFT() {
  tft.init(240, 320);   // ST7789 320x240: init(240, 320)
  tft.setRotation(1);   // landscape
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tftLogLine = 0;       // reset console cursor
}

bool fetchAndDrawFrame() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] WiFi not connected, skipping frame fetch");
    return false;
  }

  WiFiClient client;
  HTTPClient http;

  String url = "http://" + serverHost + ":" + String(serverPort) + "/frame.raw";
  Serial.print("[HTTP] GET ");
  Serial.println(url);

  if (!http.begin(client, url)) {
    Serial.println("[HTTP] http.begin failed");
    return false;  // treat as server unreachable
  }

  int httpCode = http.GET();
  Serial.printf("[HTTP] GET result code: %d\n", httpCode);

  // Server reachable, but no frame saved yet
  if (httpCode == HTTP_CODE_NOT_FOUND) {   // 404
    Serial.println("[HTTP] No frame on server yet (404), skipping draw");
    http.end();
    return true;   // SERVER IS OK, just no frame
  }

  // Any other non-200 is treated as "server problem / unreachable"
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[HTTP] GET failed, code=%d\n", httpCode);
    http.end();
    return false;
  }

  // --- At this point: HTTP 200 OK, stream the frame ---

  WiFiClient *stream = http.getStreamPtr();

  const int width = 320;
  const int height = 240;
  const int expectedBytes = width * height * 2;
  int bytesRead = 0;

  Serial.println("[HTTP] Drawing frame to TFT...");
  tft.fillScreen(ST77XX_BLACK);

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      uint8_t hi, lo;

      // Wait until we have 2 bytes available (with timeout)
      int waited = 0;
      while (stream->available() < 2) {
        delay(1);
        waited++;
        if (waited > 2000) {  // ~2s per row
          Serial.println("[HTTP] Timeout waiting for frame data");
          http.end();
          return false;
        }
      }

      if (stream->readBytes(&hi, 1) != 1 || stream->readBytes(&lo, 1) != 1) {
        Serial.println("[HTTP] Stream ended early while reading frame");
        http.end();
        return false;
      }

      bytesRead += 2;
      uint16_t color = (hi << 8) | lo;
      tft.drawPixel(x, y, color);
    }
    yield();  // let WiFi / watchdog breathe
  }

  Serial.printf("[HTTP] Frame drawn, bytesRead=%d (expected %d)\n",
                bytesRead, expectedBytes);

  http.end();
  return true;
}

// ---------- SETUP / LOOP ----------

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[BOOT] TeleDraw starting");

  setupTFT();
  tftLog("[BOOT] TeleDraw starting");

  // --- Filesystem ---
  tftLog("[FS] Mounting LittleFS...");
  if (!LittleFS.begin()) {
    Serial.println("[FS] LittleFS begin failed, formatting...");
    tftLog("[FS] Mount failed, formatting...");
    LittleFS.format();
    if (LittleFS.begin()) {
      tftLog("[FS] LittleFS OK after format");
    } else {
      tftLog("[FS] LittleFS ERROR");
    }
  } else {
    tftLog("[FS] LittleFS mounted");
  }

  // --- Config ---
  tftLog("[CFG] Loading config...");
  bool haveConfig = loadConfig();
  if (!haveConfig) {
    Serial.println("[BOOT] No config, entering AP setup");
    tftLog("[CFG] No config");
    tftLog("[AP] Starting setup portal");
    startConfigPortal();   // never returns
  } else {
    tftLog("[CFG] Config OK");
  }

  // --- WiFi ---
  tftLog("[WiFi] Connecting...");
  if (!connectWiFi()) {
    Serial.println("[BOOT] WiFi failed with saved config, entering AP setup");
    tftLog("[WiFi] Connect failed");
    tftLog("[AP] Setup mode");
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(10, 10);
    tft.println("WiFi failed");
    tft.setCursor(10, 30);
    tft.println("AP setup mode");

    startConfigPortal();   // never returns
  }

  tftLog("[WiFi] Connected");
  tftLog("[HTTP] Fetching frame.raw...");

  bool frameOK = fetchAndDrawFrame();
  if (!frameOK) {
    Serial.println("[BOOT] Server unreachable or frame fetch failed, entering AP setup");
    tftLog("[HTTP] Can't connect to the server");
    tftLog("[WiFi] AP setup mode");
    // Now go back to AP portal to re-enter WiFi + server details
    startConfigPortal();   // never returns
  } 
  setupWS();
}

void loop() {
  webSocket.loop();

  // Periodically check WiFi status (every 10s)
  unsigned long now = millis();
  if (now - lastWifiCheck > 10000) {
    lastWifiCheck = now;

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Lost connection, trying to reconnect...");
      WiFi.disconnect();
      connectWiFi();   // your existing function
      // Re-init WS to be safe
      webSocket.disconnect();
      setupWS();
    }
  }
}
