#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <qrcode.h>
#include <time.h>
#include "web_ui.h"

static constexpr int PIN_BL = 21;
static constexpr int PIN_LED_R = 4;
static constexpr int PIN_LED_G = 16;
static constexpr int PIN_LED_B = 17;
static constexpr int PIN_TOUCH_CS = 33;
static constexpr int PIN_TOUCH_IRQ = 36;

static const uint16_t C_BG = 0x0862;
static const uint16_t C_PANEL = 0x11E6;
static const uint16_t C_LINE = 0x2A6B;
static const uint16_t C_TEXT = 0xFFFF;
static const uint16_t C_DIM = 0x94D3;
static const uint16_t C_ACC = 0x05DF;
static const uint16_t C_OK = 0x06EE;
static const uint16_t C_WARN = 0xFD60;
static const uint16_t C_BAD = 0xFACD;
static const uint16_t C_SUN = 0xFEC0;
static const uint16_t C_CLOUD = 0xBDF7;
static const uint16_t C_RAIN = 0x4D7F;

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite ticker(&tft);
bool tickerReady = false;
SPIClass touchSPI(HSPI);
WebServer server(80);
DNSServer dnsServer;
WiFiUDP udp;
Preferences prefs;

struct Config {
  String ssid;
  String pass;
  String city;
  String tz;
  String feed;
  String units;
  float lat;
  float lon;
  uint8_t brightness;
  bool invert;
  uint8_t rotation;
};

struct Stats {
  float cpu = -1;
  float cpuTemp = -1;
  float ram = -1;
  float ramUsed = -1;
  float ramTotal = -1;
  float gpu = -1;
  float gpuTemp = -1;
  float gpuMemUsed = -1;
  float gpuMemTotal = -1;
  String host;
  bool valid = false;
  uint32_t at = 0;
};

struct Feed {
  String city;
  String source;
  String units;
  String headlines[8];
  String forecastDay[4];
  float forecastHigh[4];
  float forecastLow[4];
  int forecastCode[4];
  int forecastCount = 0;
  int count = 0;
  float temp = 0;
  float feels = 0;
  float humidity = -1;
  float wind = -1;
  int code = -1;
  bool valid = false;
  uint32_t at = 0;
};

Config cfg;
Stats stats;
Feed feed;

bool apMode = false;
uint8_t page = 0;
bool rebootPending = false;
uint32_t rebootAt = 0;
uint32_t bootMs = 0;
uint32_t lastWifiCheck = 0;
uint32_t lastTouchMs = 0;
uint32_t lastPageChange = 0;
uint32_t lastClockMs = 0;
uint32_t lastStatsMs = 0;
uint32_t lastTickerMs = 0;
uint32_t lastNewsMs = 0;
uint32_t lastForecastMs = 0;
uint32_t lastSysMs = 0;
int lastMinute = -1;
int lastDay = -1;
bool feedDirty = true;
String tickerText;
int tickerX = 320;
int tickerW = 0;

static const char* DEFAULT_TZ = "GMT0BST,M3.5.0/1,M10.5.0";
static const char* DEFAULT_FEED = "http://feeds.bbci.co.uk/news/rss.xml";

void drawPage(bool full);
void drawDashboard();
void drawStats();
void drawClock();
void drawDate();
void drawWeather();
void drawStatusLine();
void drawLiveButton();
void drawNewsPage();
void drawForecastPage();
void drawSystemPage();
void drawQr(const String& text, int x, int y);

String fitText(const String& s, int maxW, uint8_t font) {
  if (tft.textWidth(s, font) <= maxW) return s;
  String out = s;
  while (out.length() > 1 && tft.textWidth(out + "..", font) > maxW) out.remove(out.length() - 1);
  return out + "..";
}

void loadConfig() {
  prefs.begin("cyd", true);
  cfg.ssid = prefs.getString("ssid", "");
  cfg.pass = prefs.getString("pass", "");
  cfg.city = prefs.getString("city", "London");
  cfg.lat = prefs.getFloat("lat", 51.5072f);
  cfg.lon = prefs.getFloat("lon", -0.1276f);
  cfg.units = prefs.getString("units", "metric");
  cfg.tz = prefs.getString("tz", DEFAULT_TZ);
  cfg.feed = prefs.getString("feed", DEFAULT_FEED);
  cfg.brightness = prefs.getUChar("bri", 255);
  cfg.invert = prefs.getBool("inv", false);
  cfg.rotation = prefs.getUChar("rot", 1);
  prefs.end();
  if (cfg.rotation != 3) cfg.rotation = 1;
  if (cfg.brightness < 10) cfg.brightness = 10;
}

void saveConfig() {
  prefs.begin("cyd", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("city", cfg.city);
  prefs.putFloat("lat", cfg.lat);
  prefs.putFloat("lon", cfg.lon);
  prefs.putString("units", cfg.units);
  prefs.putString("tz", cfg.tz);
  prefs.putString("feed", cfg.feed);
  prefs.putUChar("bri", cfg.brightness);
  prefs.putBool("inv", cfg.invert);
  prefs.putUChar("rot", cfg.rotation);
  prefs.end();
}

void applyDisplay() {
  tft.setRotation(cfg.rotation == 3 ? 3 : 1);
  tft.invertDisplay(cfg.invert);
  analogWrite(PIN_BL, cfg.brightness);
}

void setLed(bool r, bool g, bool b) {
  digitalWrite(PIN_LED_R, r ? LOW : HIGH);
  digitalWrite(PIN_LED_G, g ? LOW : HIGH);
  digitalWrite(PIN_LED_B, b ? LOW : HIGH);
}

void ledForLoad(float pct) {
  if (pct < 0) { setLed(false, false, false); return; }
  if (pct < 50) setLed(false, true, false);
  else if (pct < 80) setLed(true, true, false);
  else setLed(true, false, false);
}

uint16_t barColor(float pct) {
  if (pct < 0) return C_LINE;
  if (pct < 50) return C_OK;
  if (pct < 80) return C_WARN;
  return C_BAD;
}

const char* weatherText(int code) {
  if (code == 0) return "Clear";
  if (code == 1) return "Mostly clear";
  if (code == 2) return "Partly cloudy";
  if (code == 3) return "Overcast";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 57) return "Drizzle";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Showers";
  if (code == 85 || code == 86) return "Snow showers";
  if (code >= 95) return "Thunderstorm";
  return "—";
}

String fmtTemp(float v) {
  if (v < -100) return "--";
  return String((int)roundf(v)) + "C";
}

String fmtWeatherTemp(float v) {
  if (v < -100) return "--";
  return String((int)roundf(v)) + (feed.units == "imperial" ? "F" : "C");
}

String fmtUptime(uint32_t sec) {
  uint32_t d = sec / 86400, h = (sec % 86400) / 3600, m = (sec % 3600) / 60;
  char b[24];
  if (d) snprintf(b, sizeof(b), "%ud %uh", d, h);
  else if (h) snprintf(b, sizeof(b), "%uh %um", h, m);
  else snprintf(b, sizeof(b), "%um", m);
  return String(b);
}

String ipString() {
  return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

bool agentOnline() {
  return stats.valid && (millis() - stats.at) < 15000;
}

bool feedFresh() {
  return feed.valid && (millis() - feed.at) < 3600000;
}

void buildTicker() {
  tickerText = "";
  for (int i = 0; i < feed.count; i++) {
    if (i) tickerText += "     \xB7     ";
    tickerText += feed.headlines[i];
  }
  tickerW = tft.textWidth(tickerText, 4);
  tickerX = 320;
}

// ---------------------------------------------------------------- display

void drawBarRow(int y, const char* label, float pct, const String& value) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(label, 8, y);

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString(value, 312, y);

  int bx = 8, by = y + 18, bw = 304, bh = 10;
  tft.fillRoundRect(bx, by, bw, bh, 5, C_PANEL);
  if (pct > 0) {
    int fw = (int)(bw * (pct > 100 ? 100 : pct) / 100.0f);
    if (fw < bh) fw = bh;
    tft.fillRoundRect(bx, by, fw, bh, 5, barColor(pct));
  }
}

void drawStats() {
  tft.fillRect(0, 56, 320, 112, C_BG);
  String cpuVal = stats.cpu < 0 ? "--" : String((int)roundf(stats.cpu)) + "%";
  if (stats.cpuTemp >= 0) cpuVal += "  " + fmtTemp(stats.cpuTemp) + "C";
  String gpuVal = stats.gpu < 0 ? "--" : String((int)roundf(stats.gpu)) + "%";
  if (stats.gpuTemp >= 0) gpuVal += "  " + fmtTemp(stats.gpuTemp) + "C";
  String ramVal = stats.ram < 0 ? "--" : String((int)roundf(stats.ram)) + "%";
  if (stats.ramUsed >= 0 && stats.ramTotal > 0) {
    ramVal += "  " + String(stats.ramUsed, 1) + "/" + String(stats.ramTotal, 1) + "G";
  }

  drawBarRow(58, "CPU", stats.cpu, cpuVal);
  drawBarRow(96, "GPU", stats.gpu, gpuVal);
  drawBarRow(134, "RAM", stats.ram, ramVal);

  ledForLoad(stats.cpu);
}

void drawClock() {
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[8];
  if (t < 1700000000) strcpy(buf, "--:--");
  else strftime(buf, sizeof(buf), "%H:%M", &tm);
  tft.fillRect(6, 0, 150, 34, C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString(buf, 8, 0);
}

void drawDate() {
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[24];
  if (t < 1700000000) strcpy(buf, "waiting for time");
  else strftime(buf, sizeof(buf), "%a %d %b", &tm);
  tft.fillRect(6, 34, 190, 18, C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(buf, 8, 34);
}

void drawWeather() {
  tft.fillRect(196, 0, 116, 52, C_BG);
  tft.setTextDatum(TR_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(feedFresh() ? C_TEXT : C_DIM, C_BG);
  tft.drawString(feedFresh() ? fmtWeatherTemp(feed.temp) : "--", 312, 0);
  tft.setTextFont(2);
  tft.setTextColor(C_DIM, C_BG);
  String city = feed.city.length() ? feed.city : cfg.city;
  tft.drawString(fitText(city, 116, 2), 312, 32);
}

void drawStatusLine() {
  tft.fillRect(0, 170, 320, 10, C_BG);
  tft.fillRect(140, 170, 180, 10, C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(C_ACC, C_BG);
  tft.drawString("NEWS", 8, 171);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(feed.source.length() ? feed.source : "feed", 44, 171);

  tft.setTextDatum(TR_DATUM);
  String s = ipString();
  if (!apMode) s += "  " + String(WiFi.RSSI()) + "dBm";
  tft.drawString(s, 300, 171);
  tft.fillCircle(308, 175, 4, agentOnline() ? C_OK : C_BAD);
}

void drawTickerFrame() {
  if (!tickerReady) {
    tft.fillRect(0, 184, 320, 32, C_BG);
    if (tickerText.length()) {
      tft.setTextDatum(TL_DATUM);
      tft.setTextFont(4);
      tft.setTextColor(C_TEXT, C_BG);
      tft.drawString(tickerText, tickerX, 187);
    }
    return;
  }
  ticker.fillSprite(C_BG);
  ticker.setTextDatum(TL_DATUM);
  ticker.setTextFont(4);
  ticker.setTextColor(C_TEXT, C_BG);
  if (tickerText.length()) ticker.drawString(tickerText, tickerX, 3);
  ticker.pushSprite(0, 184);
}

void drawLiveButton() {
  tft.fillRoundRect(0, 216, 44, 24, 6, C_ACC);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_BLACK, C_ACC);
  tft.drawString("LIVE", 7, 220);
}

void drawTickerStep() {
  if (!tickerText.length()) return;
  if (millis() - lastTickerMs < 60) return;
  lastTickerMs = millis();
  drawTickerFrame();
  tickerX -= 3;
  if (tickerX < -tickerW) tickerX = 320;
}

void drawDashboard() {
  tft.fillScreen(C_BG);
  drawClock();
  drawDate();
  drawWeather();
  drawStats();
  drawStatusLine();
  if (tickerText.length()) {
    tickerW = tft.textWidth(tickerText, 4);
    tickerX = 320;
  }
  drawTickerFrame();
  drawLiveButton();
  lastMinute = -1;
  lastDay = -1;
  feedDirty = false;
}

void drawNewsPage() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 320, 26, C_PANEL);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(C_ACC, C_PANEL);
  tft.drawString(feed.source.length() ? feed.source : "NEWS", 8, 5);

  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[8];
  if (t < 1700000000) strcpy(buf, "--:--");
  else strftime(buf, sizeof(buf), "%H:%M", &tm);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString(buf, 312, 5);

  tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < 6; i++) {
    int y = 34 + i * 28;
    if (i < feed.count) {
      tft.setTextColor(C_ACC, C_BG);
      tft.drawString(String(i + 1), 8, y);
      tft.setTextColor(C_TEXT, C_BG);
      tft.drawString(fitText(feed.headlines[i], 280, 2), 26, y);
    } else {
      tft.setTextColor(C_LINE, C_BG);
      tft.drawString("—", 8, y);
    }
  }

  tft.setTextFont(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("tap to cycle pages", 8, 222);
  if (feed.valid) {
    tft.setTextDatum(TR_DATUM);
    tft.drawString(String((millis() - feed.at) / 60000) + "m ago", 312, 222);
  }
}

void drawSunIcon(int cx, int cy, int r) {
  tft.fillCircle(cx, cy, r, C_SUN);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4.0f;
    int x1 = cx + (int)(cosf(a) * (r + 3));
    int y1 = cy + (int)(sinf(a) * (r + 3));
    int x2 = cx + (int)(cosf(a) * (r + 8));
    int y2 = cy + (int)(sinf(a) * (r + 8));
    tft.drawLine(x1, y1, x2, y2, C_SUN);
  }
}

void drawCloudIcon(int cx, int cy, int scale) {
  int r = 6 * scale;
  tft.fillCircle(cx - 8 * scale, cy, r, C_CLOUD);
  tft.fillCircle(cx, cy - 5 * scale, 8 * scale, C_CLOUD);
  tft.fillCircle(cx + 9 * scale, cy, 7 * scale, C_CLOUD);
  tft.fillRoundRect(cx - 15 * scale, cy, 30 * scale, 8 * scale, 3 * scale, C_CLOUD);
}

void drawWeatherIcon(int code, int cx, int cy, int scale) {
  if (code == 0) {
    drawSunIcon(cx, cy, 7 * scale);
    return;
  }
  if (code == 1 || code == 2) {
    drawSunIcon(cx - 8 * scale, cy - 7 * scale, 5 * scale);
    drawCloudIcon(cx + 4 * scale, cy + 3 * scale, scale);
    return;
  }
  if (code == 45 || code == 48) {
    drawCloudIcon(cx, cy - 5 * scale, scale);
    for (int i = 0; i < 3; i++) {
      int y = cy + (7 + i * 5) * scale;
      tft.drawFastHLine(cx - 14 * scale, y, 28 * scale, C_DIM);
    }
    return;
  }

  drawCloudIcon(cx, cy - 5 * scale, scale);
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    for (int i = -1; i <= 1; i++) {
      int x = cx + i * 9 * scale;
      int y = cy + 8 * scale + (i == 0 ? 3 * scale : 0);
      tft.drawLine(x, y, x - 3 * scale, y + 7 * scale, C_RAIN);
    }
  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
    for (int i = -1; i <= 1; i++) {
      int x = cx + i * 9 * scale;
      int y = cy + 13 * scale;
      tft.drawLine(x - 3 * scale, y, x + 3 * scale, y, TFT_WHITE);
      tft.drawLine(x, y - 3 * scale, x, y + 3 * scale, TFT_WHITE);
      tft.drawLine(x - 2 * scale, y - 2 * scale, x + 2 * scale, y + 2 * scale, TFT_WHITE);
    }
  } else if (code >= 95) {
    int x = cx - 2 * scale;
    int y = cy + 5 * scale;
    tft.fillTriangle(x, y, x + 7 * scale, y, x + 1 * scale, y + 9 * scale, C_WARN);
    tft.fillTriangle(x + scale, y + 7 * scale, x + 5 * scale, y + 7 * scale,
                     x - 3 * scale, y + 16 * scale, C_WARN);
  }
}

void drawForecastPage() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 320, 26, C_PANEL);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(C_ACC, C_PANEL);
  tft.drawString("FORECAST", 8, 5);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString(fitText(feed.city.length() ? feed.city : cfg.city, 190, 2), 312, 5);

  if (!feed.valid) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString("Waiting for local weather...", 160, 120);
    return;
  }

  drawWeatherIcon(feed.code, 42, 62, 2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString(fmtWeatherTemp(feed.temp), 88, 39);
  tft.setTextFont(2);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(weatherText(feed.code), 89, 69);

  String windUnit = feed.units == "imperial" ? "mph" : "km/h";
  String details = "Feels " + fmtWeatherTemp(feed.feels) + "   Hum " +
                   String((int)roundf(feed.humidity)) + "%   Wind " +
                   String((int)roundf(feed.wind)) + " " + windUnit;
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(details, 160, 103);

  for (int i = 0; i < 4; i++) {
    int x = 5 + i * 79;
    tft.fillRoundRect(x, 120, 73, 96, 7, C_PANEL);
    if (i >= feed.forecastCount) continue;

    tft.setTextDatum(TC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(i == 0 ? C_ACC : C_TEXT, C_PANEL);
    tft.drawString(feed.forecastDay[i], x + 36, 126);
    drawWeatherIcon(feed.forecastCode[i], x + 36, 161, 1);

    tft.setTextDatum(TC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(C_TEXT, C_PANEL);
    String temps = String((int)roundf(feed.forecastHigh[i])) + "/" +
                   String((int)roundf(feed.forecastLow[i])) +
                   (feed.units == "imperial" ? "F" : "C");
    tft.drawString(temps, x + 36, 191);
  }

  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("tap to cycle pages", 8, 226);
  if (feed.valid) {
    tft.setTextDatum(TR_DATUM);
    tft.drawString(String((millis() - feed.at) / 60000) + "m ago", 312, 226);
  }
}

void drawQr(const String& text, int x, int y) {
  QRCode qr;
  uint8_t buf[512];
  if (qrcode_initText(&qr, buf, 3, ECC_LOW, text.c_str()) != 0) return;
  int scale = 4;
  int size = qr.size * scale;
  tft.fillRect(x - 4, y - 4, size + 8, size + 8, TFT_WHITE);
  for (int yy = 0; yy < qr.size; yy++) {
    for (int xx = 0; xx < qr.size; xx++) {
      if (qrcode_getModule(&qr, xx, yy)) {
        tft.fillRect(x + xx * scale, y + yy * scale, scale, scale, TFT_BLACK);
      }
    }
  }
}

void drawSystemPage() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 320, 26, C_PANEL);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(C_ACC, C_PANEL);
  tft.drawString("SYSTEM", 8, 5);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString(fmtUptime(millis() / 1000), 312, 5);

  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  const int x = 8, w = 180;
  int y = 34;
  struct Row { String label; String value; };
  Row rows[] = {
    {"IP", ipString()},
    {"WiFi", apMode ? String("CYD-Setup (AP)") : cfg.ssid},
    {"RSSI", apMode ? String("—") : String(WiFi.RSSI()) + " dBm"},
    {"Heap", String(ESP.getFreeHeap() / 1024) + " KB"},
    {"Flash", String(ESP.getFlashChipSize() / 1048576) + " MB"},
    {"Chip", String((int)roundf(temperatureRead())) + "C"},
    {"Agent", agentOnline() ? String("online " + String((millis() - stats.at) / 1000) + "s") : String("offline")},
    {"MAC", WiFi.macAddress()},
  };
  for (auto& r : rows) {
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString(r.label, x, y);
    tft.setTextColor(C_TEXT, C_BG);
    tft.drawString(fitText(r.value, w - 40, 2), x + 42, y);
    y += 19;
  }

  tft.setTextFont(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("Scan to open the setup page", 196, 78);
  drawQr("http://" + ipString() + "/", 198, 94);

  tft.drawString("tap to cycle pages", 8, 222);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("cyd.local", 312, 222);
}

void drawPage(bool full) {
  if (page == 0) drawDashboard();
  else if (page == 1) drawNewsPage();
  else if (page == 2) drawForecastPage();
  else drawSystemPage();
  lastStatsMs = millis();
}

// ---------------------------------------------------------------- web

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleNotFound() {
  server.sendHeader("Location", "http://" + ipString() + "/", true);
  server.send(302, "text/plain", "");
}

void handleStatus() {
  JsonDocument doc;
  doc["ip"] = ipString();
  doc["mode"] = apMode ? "ap" : "sta";
  doc["ssid"] = apMode ? "CYD-Setup" : WiFi.SSID();
  doc["rssi"] = apMode ? 0 : WiFi.RSSI();
  doc["heap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;
  doc["chipTemp"] = temperatureRead();
  doc["agent"] = agentOnline();
  doc["agentAge"] = stats.valid ? (millis() - stats.at) / 1000 : 0;
  doc["feedAge"] = feed.valid ? (millis() - feed.at) / 1000 : 0;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleConfigGet() {
  JsonDocument doc;
  doc["ssid"] = cfg.ssid;
  doc["city"] = cfg.city;
  doc["lat"] = cfg.lat;
  doc["lon"] = cfg.lon;
  doc["units"] = cfg.units;
  doc["tz"] = cfg.tz;
  doc["feed"] = cfg.feed;
  doc["brightness"] = cfg.brightness;
  doc["invert"] = cfg.invert;
  doc["rotation"] = cfg.rotation;
  doc["ip"] = ipString();
  doc["mode"] = apMode ? "ap" : "sta";
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleConfigPost() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  bool tzChanged = false;
  if (doc["city"].is<const char*>()) cfg.city = doc["city"].as<String>();
  if (doc["lat"].is<float>() || doc["lat"].is<int>()) cfg.lat = doc["lat"].as<float>();
  if (doc["lon"].is<float>() || doc["lon"].is<int>()) cfg.lon = doc["lon"].as<float>();
  if (doc["units"].is<const char*>()) cfg.units = doc["units"].as<String>();
  if (doc["tz"].is<const char*>()) {
    String tz = doc["tz"].as<String>();
    if (tz != cfg.tz) tzChanged = true;
    cfg.tz = tz;
  }
  if (doc["feed"].is<const char*>()) cfg.feed = doc["feed"].as<String>();
  if (doc["brightness"].is<int>()) cfg.brightness = constrain(doc["brightness"].as<int>(), 10, 255);
  if (doc["invert"].is<bool>()) cfg.invert = doc["invert"].as<bool>();
  if (doc["rotation"].is<int>()) cfg.rotation = doc["rotation"].as<int>() == 3 ? 3 : 1;

  saveConfig();
  applyDisplay();
  if (tzChanged && !apMode) configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.google.com");
  feedDirty = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleWifi() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  String ssid = doc["ssid"] | "";
  String pass = doc["pass"] | "";
  if (!ssid.length()) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  cfg.ssid = ssid;
  cfg.pass = pass;
  saveConfig();
  server.send(200, "application/json", "{\"ok\":true}");
  rebootAt = millis() + 1500;
  rebootPending = true;
}

void handleScan() {
  int n = WiFi.scanNetworks();
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  int count = 0;
  for (int i = 0; i < n && count < 25; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = ssid;
    o["rssi"] = WiFi.RSSI(i);
    o["enc"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    count++;
  }
  WiFi.scanDelete();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleStats() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  stats.cpu = doc["cpu"] | -1.0f;
  stats.cpuTemp = doc["cpu_temp"] | -1.0f;
  stats.ram = doc["ram"] | -1.0f;
  stats.ramUsed = doc["ram_used"] | -1.0f;
  stats.ramTotal = doc["ram_total"] | -1.0f;
  stats.gpu = doc["gpu"] | -1.0f;
  stats.gpuTemp = doc["gpu_temp"] | -1.0f;
  stats.gpuMemUsed = doc["gpu_mem_used"] | -1.0f;
  stats.gpuMemTotal = doc["gpu_mem_total"] | -1.0f;
  stats.host = doc["host"] | "";
  stats.valid = true;
  stats.at = millis();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleFeed() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  feed.city = doc["city"] | "";
  feed.source = doc["source"] | "";
  feed.units = doc["units"] | "metric";
  feed.temp = doc["temp"] | 0.0f;
  feed.feels = doc["feels"] | 0.0f;
  feed.humidity = doc["humidity"] | -1.0f;
  feed.wind = doc["wind"] | -1.0f;
  feed.code = doc["code"] | -1;
  feed.count = 0;
  JsonArray arr = doc["headlines"].as<JsonArray>();
  for (JsonVariant v : arr) {
    if (feed.count >= 8) break;
    String h = v.as<String>();
    h.trim();
    if (h.length()) feed.headlines[feed.count++] = h;
  }
  feed.forecastCount = 0;
  JsonArray forecast = doc["forecast"].as<JsonArray>();
  for (JsonObject day : forecast) {
    if (feed.forecastCount >= 4) break;
    int i = feed.forecastCount++;
    feed.forecastDay[i] = day["day"] | "--";
    feed.forecastHigh[i] = day["high"] | 0.0f;
    feed.forecastLow[i] = day["low"] | 0.0f;
    feed.forecastCode[i] = day["code"] | -1;
  }
  feed.valid = true;
  feed.at = millis();
  buildTicker();
  feedDirty = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleReboot() {
  server.send(200, "application/json", "{\"ok\":true}");
  rebootAt = millis() + 600;
  rebootPending = true;
}

void handleReset() {
  prefs.begin("cyd", false);
  prefs.clear();
  prefs.end();
  server.send(200, "application/json", "{\"ok\":true}");
  rebootAt = millis() + 600;
  rebootPending = true;
}

void setupServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/wifi", HTTP_POST, handleWifi);
  server.on("/api/stats", HTTP_POST, handleStats);
  server.on("/api/feed", HTTP_POST, handleFeed);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.onNotFound(handleNotFound);
  server.begin();
}

// ---------------------------------------------------------------- net

bool tryConnect(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("cyd-dashboard");
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(200);
  }
  return false;
}

void startPortal() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("CYD-Setup");
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.printf("AP mode, setup at http://%s/\n", WiFi.softAPIP().toString().c_str());
}

void handleUdp() {
  int sz = udp.parsePacket();
  if (!sz) return;
  char buf[64] = {0};
  int n = udp.read(buf, sizeof(buf) - 1);
  if (n <= 0) return;
  if (strstr(buf, "CYD_DISCOVER")) {
    String reply = "{\"ip\":\"" + ipString() + "\",\"name\":\"cyd-dashboard\"}";
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.write((const uint8_t*)reply.c_str(), reply.length());
    udp.endPacket();
  }
}

void splash(const String& msg) {
  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(C_ACC, C_BG);
  tft.drawString("CYD Dashboard", 12, 88);
  tft.setTextFont(2);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(msg, 12, 128);
}

// ---------------------------------------------------------------- main

bool touchRead() {
  touchSPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_TOUCH_CS, LOW);
  touchSPI.transfer(0xB1);
  int16_t z1 = touchSPI.transfer16(0xC1) >> 3;
  int z = z1 + 4095;
  int16_t z2 = touchSPI.transfer16(0x91) >> 3;
  z -= z2;
  touchSPI.transfer16(0x91);
  touchSPI.transfer16(0xD1);
  touchSPI.transfer16(0x91);
  touchSPI.transfer16(0xD1);
  touchSPI.transfer16(0xD0);
  touchSPI.transfer16(0);
  digitalWrite(PIN_TOUCH_CS, HIGH);
  touchSPI.endTransaction();
  return z >= 400;
}

void handleTouch() {
  if (millis() - lastTouchMs < 500) return;
  if (!touchRead()) return;
  lastTouchMs = millis();
  lastPageChange = millis();
  page = (page + 1) % 4;
  drawPage(true);
}

void updateDisplay() {
  uint32_t now = millis();
  if (page == 0) {
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_min != lastMinute) {
      lastMinute = tm.tm_min;
      drawClock();
    }
    if (tm.tm_mday != lastDay) {
      lastDay = tm.tm_mday;
      drawDate();
    }
    if (now - lastStatsMs >= 1000) {
      lastStatsMs = now;
      drawStats();
      drawStatusLine();
    }
    if (feedDirty) {
      feedDirty = false;
      drawWeather();
    }
    drawTickerStep();
    if (now - lastPageChange > 30000) lastPageChange = now;
  } else if (page == 1) {
    if (now - lastNewsMs >= 5000) {
      lastNewsMs = now;
      drawNewsPage();
    }
    if (now - lastPageChange > 30000) {
      page = 0;
      lastPageChange = now;
      drawPage(true);
    }
  } else if (page == 2) {
    if (now - lastForecastMs >= 30000) {
      lastForecastMs = now;
      drawForecastPage();
    }
    if (now - lastPageChange > 30000) {
      page = 0;
      lastPageChange = now;
      drawPage(true);
    }
  } else {
    if (now - lastSysMs >= 2000) {
      lastSysMs = now;
      drawSystemPage();
    }
    if (now - lastPageChange > 30000) {
      page = 0;
      lastPageChange = now;
      drawPage(true);
    }
  }
}

void setup() {
  Serial.begin(115200);
  bootMs = millis();
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  setLed(false, false, false);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(C_BG);
  loadConfig();
  applyDisplay();

  splash("Connecting to WiFi...");
  if (cfg.ssid.length() && tryConnect(15000)) {
    apMode = false;
    configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.google.com");
    MDNS.begin("cyd");
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Connected, IP %s\n", WiFi.localIP().toString().c_str());
  } else {
    startPortal();
  }

  setupServer();
  udp.begin(8266);

  touchSPI.begin(25, 39, 32, PIN_TOUCH_CS);
  pinMode(PIN_TOUCH_CS, OUTPUT);
  digitalWrite(PIN_TOUCH_CS, HIGH);

  ticker.setColorDepth(16);
  tickerReady = ticker.createSprite(320, 32) != nullptr;
  Serial.printf("Ticker sprite: %s\n", tickerReady ? "ok" : "FAILED");

  page = 0;
  drawPage(true);
  lastPageChange = millis();
}

String serialBuf;

void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (serialBuf.length() < 200) serialBuf += c;
      continue;
    }
    String line = serialBuf;
    serialBuf = "";
    line.trim();
    if (line.startsWith("WIFI ")) {
      String arg = line.substring(5);
      int sep = arg.indexOf('|');
      if (sep > 0) {
        cfg.ssid = arg.substring(0, sep);
        cfg.pass = arg.substring(sep + 1);
        saveConfig();
        Serial.printf("Saved WiFi '%s', rebooting...\n", cfg.ssid.c_str());
        delay(300);
        ESP.restart();
      }
    } else if (line == "RESET") {
      prefs.begin("cyd", false);
      prefs.clear();
      prefs.end();
      Serial.println("Settings cleared, rebooting...");
      delay(300);
      ESP.restart();
    } else if (line == "STATUS") {
      Serial.printf("mode=%s ip=%s ssid=%s heap=%u uptime=%lu\n",
                    apMode ? "ap" : "sta", ipString().c_str(),
                    apMode ? "CYD-Setup" : WiFi.SSID().c_str(),
                    ESP.getFreeHeap(), (unsigned long)(millis() / 1000));
    }
  }
}

void loop() {
  server.handleClient();
  if (apMode) dnsServer.processNextRequest();
  handleUdp();
  handleSerial();

  if (rebootPending && (int32_t)(millis() - rebootAt) >= 0) {
    Serial.println("Rebooting...");
    delay(100);
    ESP.restart();
  }

  handleTouch();

  if (!apMode && WiFi.status() != WL_CONNECTED && millis() - lastWifiCheck > 15000) {
    lastWifiCheck = millis();
    Serial.println("WiFi lost, reconnecting...");
    WiFi.reconnect();
  }

  updateDisplay();
  delay(2);
}
