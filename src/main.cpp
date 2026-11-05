#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>
#include <qrcode.h>
#include <time.h>
#include "web_ui.h"

static constexpr int PIN_BL = 21;
static constexpr int PIN_LED_R = 4;
static constexpr int PIN_LED_G = 16;
static constexpr int PIN_LED_B = 17;
static constexpr int PIN_TOUCH_CS = 33;
static constexpr int PIN_TOUCH_IRQ = 36;
static constexpr int PIN_LDR = 34;
static constexpr int MAX_HEADLINES = 32;
static constexpr int MAX_NEWS_ARTICLES = 24;
static constexpr int NEWS_IMAGE_W = 96;
static constexpr int NEWS_IMAGE_H = 54;
static constexpr int NEWS_IMAGE_PIXELS = NEWS_IMAGE_W * NEWS_IMAGE_H;
static constexpr int ASSET_IMAGE_W = 54;
static constexpr int ASSET_IMAGE_H = 54;
static constexpr int ASSET_IMAGE_PIXELS = ASSET_IMAGE_W * ASSET_IMAGE_H;
static constexpr uint16_t ASSET_TRANSPARENT = 0x0001;
static constexpr uint32_t NEWS_CACHE_MAX_AGE_SEC = 3UL * 24UL * 60UL * 60UL;
static constexpr uint32_t TIDE_CACHE_MAX_AGE_SEC = 6UL * 60UL * 60UL;
static constexpr uint32_t MARINE_CACHE_MAX_AGE_SEC = 24UL * 60UL * 60UL;
static constexpr int MAX_TIDE_POINTS = 49;
static constexpr int MAX_TIDE_FUTURE = 96;
static constexpr int MAX_TIDE_EVENTS = 6;
static constexpr int MAX_TIDE_FUTURE_EVENTS = 6;

static uint16_t C_BG = 0x0862;
static uint16_t C_PANEL = 0x11E6;
static uint16_t C_LINE = 0x2A6B;
static uint16_t C_TEXT = 0xFFFF;
static uint16_t C_DIM = 0x94D3;
static uint16_t C_ACC = 0x05DF;
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
  String tidePort;
  String tideStation;
  String units;
  float lat;
  float lon;
  uint8_t brightness;
  bool invert;
  uint8_t rotation;
  bool autoBrightness;
  bool sleepNoAgent;
  bool autoRotate;
  uint16_t rotateMinutes;
  uint32_t colorBg, colorPanel, colorLine, colorText, colorDim, colorAccent;
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
  float disk = -1, diskUsed = -1, diskTotal = -1;
  float netDown = 0, netUp = 0;
  uint32_t hostUptime = 0;
  String host;
  bool valid = false;
  uint32_t at = 0;
};

struct NewsArticle {
  bool imageReady = false;
};

struct Feed {
  String city;
  String source;
  String units;
  String headlines[MAX_HEADLINES];
  String forecastDay[4];
  float forecastHigh[4];
  float forecastLow[4];
  int forecastCode[4];
  int forecastCount = 0;
  int count = 0;
  NewsArticle articles[MAX_NEWS_ARTICLES];
  int articleCount = 0;
  uint32_t articleAt = 0;
  float temp = 0;
  float feels = 0;
  float humidity = -1;
  float wind = -1;
  int code = -1;
  bool valid = false;
  uint32_t at = 0;
};

struct Tide {
  String station, latestTime;
  float heights[MAX_TIDE_POINTS];
  int16_t minutesAgo[MAX_TIDE_POINTS];
  String eventKind[MAX_TIDE_EVENTS], eventTime[MAX_TIDE_EVENTS];
  float eventHeight[MAX_TIDE_EVENTS];
  int16_t eventMinutesAgo[MAX_TIDE_EVENTS];
  float futureHeights[MAX_TIDE_FUTURE];
  int16_t futureMinutes[MAX_TIDE_FUTURE];
  String futureEventKind[MAX_TIDE_FUTURE_EVENTS], futureEventTime[MAX_TIDE_FUTURE_EVENTS];
  float futureEventHeight[MAX_TIDE_FUTURE_EVENTS];
  int16_t futureEventMinutes[MAX_TIDE_FUTURE_EVENTS];
  int count = 0, eventCount = 0, futureCount = 0, futureEventCount = 0;
  int16_t latestAge = 0;
  float latest = 0;
  float current = 0;
  bool hasCurrent = false;
  bool valid = false;
  uint32_t at = 0;
};

struct Marine {
  String station, windUnit, sunrise, sunset, moon;
  float wind = -1, windDirection = -1, windGust = -1;
  float waveHeight = -1, wavePeriod = -1, seaTemp = -100;
  float currentSpeed = -1, currentDirection = -1;
  int moonIllumination = -1;
  bool valid = false;
  uint32_t at = 0;
};

Config cfg;
Stats stats;
Feed feed;
uint16_t newsImageBuffer[NEWS_IMAGE_PIXELS];
uint16_t assetImageBuffer[ASSET_IMAGE_PIXELS];
bool newsCardReady = false;
SPIClass newsCardSPI(VSPI);
Tide tide;
Marine marine;

bool apMode = false;
uint8_t page = 0;
bool rebootPending = false;
uint32_t rebootAt = 0;
uint32_t bootMs = 0;
uint32_t lastWifiCheck = 0;
uint32_t lastLightMs = 0;
uint32_t lastTouchMs = 0;
uint32_t lastPageChange = 0;
uint32_t lastClockMs = 0;
uint32_t lastStatsMs = 0;
uint32_t lastTickerMs = 0;
uint32_t lastNewsMs = 0;
uint32_t lastNewsArticleSlot = 0;
uint32_t lastNewsImageMs = 0;
uint32_t lastForecastMs = 0;
uint32_t lastTideMs = 0, lastMarineMs = 0, lastPcMs = 0;
uint32_t lastSysMs = 0;
int lastMinute = -1;
int lastDay = -1;
bool feedDirty = true;
String tickerText;
String tickerNextText;
int tickerX = 320;
int tickerW = 0;
int tickerNextW = 0;
int tickerIndex = 0;
int lightRaw = 0;
float smoothedLight = -1;
uint8_t appliedBrightness = 0;
bool displaySleeping = false;

static const char* DEFAULT_TZ = "GMT0BST,M3.5.0/1,M10.5.0";
static const char* DEFAULT_FEED =
    "http://feeds.bbci.co.uk/news/rss.xml\n"
    "http://rss.cnn.com/rss/edition.rss\n"
    "https://news.google.com/rss";
static const char* LEGACY_FEED = "http://feeds.bbci.co.uk/news/rss.xml";

void drawPage(bool full);
void drawDashboard();
void drawStats();
void drawClock();
void drawDate();
void drawWeather();
void drawStatusLine();
void drawNewsPage();
void drawForecastPage();
void drawTidePage();
void drawMarinePage();
void drawSunMoonPage();
void drawPcPage();
bool drawWeatherAsset(int code, int x, int y);
void drawPcMetrics();
void drawSystemPage();
void drawQr(const String& text, int x, int y);
void updateBacklight(bool force = false);

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
  cfg.tidePort = prefs.getString("tidePort", "Heysham");
  cfg.tideStation = prefs.getString("tideStn", "E73424");
  if (cfg.feed == LEGACY_FEED) cfg.feed = DEFAULT_FEED;
  cfg.brightness = prefs.getUChar("bri", 255);
  cfg.autoBrightness = prefs.getBool("autoBri", false);
  cfg.sleepNoAgent = prefs.getBool("sleepAgent", false);
  cfg.autoRotate = prefs.getBool("autoRotate", false);
  cfg.rotateMinutes = prefs.getUShort("rotateMin", 5);
  cfg.colorBg = prefs.getUInt("colBg", 0x08621B);
  cfg.colorPanel = prefs.getUInt("colPan", 0x11E63A);
  cfg.colorLine = prefs.getUInt("colLine", 0x2A6B55);
  cfg.colorText = prefs.getUInt("colText", 0xFFFFFF);
  cfg.colorDim = prefs.getUInt("colDim", 0x94D3C5);
  cfg.colorAccent = prefs.getUInt("colAcc", 0x05DFFF);
  cfg.invert = prefs.getBool("inv", false);
  cfg.rotation = prefs.getUChar("rot", 1);
  prefs.end();
  if (cfg.rotation != 3) cfg.rotation = 1;
  if (cfg.rotateMinutes != 1 && cfg.rotateMinutes != 2 && cfg.rotateMinutes != 5 && cfg.rotateMinutes != 10 && cfg.rotateMinutes != 15 && cfg.rotateMinutes != 30) cfg.rotateMinutes = 5;
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
  prefs.putString("tidePort", cfg.tidePort);
  prefs.putString("tideStn", cfg.tideStation);
  prefs.putUChar("bri", cfg.brightness);
  prefs.putBool("autoBri", cfg.autoBrightness);
  prefs.putBool("sleepAgent", cfg.sleepNoAgent);
  prefs.putBool("autoRotate", cfg.autoRotate);
  prefs.putUShort("rotateMin", cfg.rotateMinutes);
  prefs.putUInt("colBg", cfg.colorBg); prefs.putUInt("colPan", cfg.colorPanel);
  prefs.putUInt("colLine", cfg.colorLine); prefs.putUInt("colText", cfg.colorText);
  prefs.putUInt("colDim", cfg.colorDim); prefs.putUInt("colAcc", cfg.colorAccent);
  prefs.putBool("inv", cfg.invert);
  prefs.putUChar("rot", cfg.rotation);
  prefs.end();
}

void applyDisplay() {
  tft.setRotation(cfg.rotation == 3 ? 3 : 1);
  tft.invertDisplay(cfg.invert);
  C_BG = tft.color565(cfg.colorBg >> 16, cfg.colorBg >> 8, cfg.colorBg);
  C_PANEL = tft.color565(cfg.colorPanel >> 16, cfg.colorPanel >> 8, cfg.colorPanel);
  C_LINE = tft.color565(cfg.colorLine >> 16, cfg.colorLine >> 8, cfg.colorLine);
  C_TEXT = tft.color565(cfg.colorText >> 16, cfg.colorText >> 8, cfg.colorText);
  C_DIM = tft.color565(cfg.colorDim >> 16, cfg.colorDim >> 8, cfg.colorDim);
  C_ACC = tft.color565(cfg.colorAccent >> 16, cfg.colorAccent >> 8, cfg.colorAccent);
  updateBacklight(true);
}

void updateBacklight(bool force) {
  uint32_t now = millis();
  if (!force && now - lastLightMs < 250) return;
  lastLightMs = now;
  uint8_t target = cfg.brightness;
  if (cfg.autoBrightness) {
    uint32_t total = 0;
    for (int i = 0; i < 8; i++) total += analogRead(PIN_LDR);
    lightRaw = total / 8;
    if (smoothedLight < 0 || force) smoothedLight = lightRaw;
    else smoothedLight = smoothedLight * 0.9f + lightRaw * 0.1f;
    target = (uint8_t)map(constrain((int)roundf(smoothedLight), 80, 1300), 80, 1300, 255, 25);
  } else { lightRaw = analogRead(PIN_LDR); smoothedLight = lightRaw; }
  uint32_t silence = stats.valid ? now - stats.at : now - bootMs;
  displaySleeping = cfg.sleepNoAgent && silence >= 900000UL;
  if (displaySleeping) target = 0;
  int next = target;
  if (!force && cfg.autoBrightness && !displaySleeping) next = appliedBrightness + constrain((int)target - appliedBrightness, -4, 4);
  next = displaySleeping ? 0 : constrain(next, 10, 255);
  if (force || next != appliedBrightness) { appliedBrightness = (uint8_t)next; analogWrite(PIN_BL, appliedBrightness); }
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
  if (feed.count <= 0) { tickerText = ""; tickerNextText = ""; tickerW = 0; tickerNextW = 0; return; }
  tickerIndex = 0;
  tickerText = feed.headlines[tickerIndex];
  tickerW = tft.textWidth(tickerText, 4);
  tickerNextText = feed.headlines[(tickerIndex + 1) % feed.count];
  tickerNextW = tft.textWidth(tickerNextText, 4);
  tickerX = 8;
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
  tft.fillRect(0, 224, 320, 16, C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(C_ACC, C_BG);
  tft.drawString("Top Stories", 8, 228);

  tft.setTextDatum(TR_DATUM);
  String s = ipString();
  if (!apMode) s += "  " + String(WiFi.RSSI()) + "dBm";
  tft.drawString(s, 300, 228);
  tft.fillCircle(308, 232, 3, agentOnline() ? C_OK : C_BAD);
}

void drawTickerFrame() {
  if (!tickerReady) {
    tft.fillRect(0, 175, 320, 45, C_BG);
    if (tickerText.length()) {
      tft.setTextDatum(TL_DATUM);
      tft.setTextFont(4);
      tft.setTextColor(C_TEXT, C_BG);
      tft.drawString(tickerText, tickerX, 181);
      if (tickerNextText.length()) tft.drawString(tickerNextText, tickerX + tickerW + 80, 181);
    }
    return;
  }
  ticker.fillSprite(C_BG);
  ticker.setTextDatum(TL_DATUM);
  ticker.setTextFont(4);
  ticker.setTextColor(C_TEXT, C_BG);
  if (tickerText.length()) {
    ticker.drawString(tickerText, tickerX, 10);
    ticker.drawString(tickerNextText, tickerX + tickerW + 80, 10);
  }
  ticker.pushSprite(0, 175);
}

void drawTickerStep() {
  if (!tickerText.length()) return;
  if (millis() - lastTickerMs < 40) return;
  lastTickerMs = millis();
  drawTickerFrame();
  tickerX -= 4;
  if (tickerX + tickerW < 0 && feed.count > 0) {
    tickerX += tickerW + 80;
    tickerIndex = (tickerIndex + 1) % feed.count;
    tickerText = tickerNextText;
    tickerW = tickerNextW;
    tickerNextText = feed.headlines[(tickerIndex + 1) % feed.count];
    tickerNextW = tft.textWidth(tickerNextText, 4);
  }
}

void drawDashboard() {
  tft.fillScreen(C_BG);
  drawClock();
  drawDate();
  drawWeather();
  drawStats();
  drawStatusLine();
  drawTickerFrame();
  lastMinute = -1;
  lastDay = -1;
  feedDirty = false;
}

int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool decodeNewsImage(const String& encoded, uint16_t* destination) {
  size_t sourceByte = 0;
  uint8_t lowByte = 0;
  int value = 0, bits = -8;
  // The agent sends exactly one base64-encoded RGB565 little-endian pixel
  // buffer at the display size. Decode directly into the TFT buffer: an
  // extra 128x72 -> 96x54 remap both wasted RAM and made the thumbnail look
  // blocky when adjacent source pixels landed on the same output pixel.
  for (size_t i = 0; i < encoded.length() && sourceByte < NEWS_IMAGE_PIXELS * 2; i++) {
    int decoded = base64Value(encoded[i]);
    if (decoded < 0) continue;
    value = (value << 6) | decoded;
    bits += 6;
    if (bits >= 0) {
      uint8_t byte = (uint8_t)((value >> bits) & 0xFF);
      if ((sourceByte & 1) == 0) {
        lowByte = byte;
      } else {
        size_t pixel = sourceByte / 2;
        destination[pixel] = ((uint16_t)byte << 8) | lowByte;
      }
      sourceByte++;
      bits -= 8;
    }
  }
  return sourceByte == NEWS_IMAGE_PIXELS * 2;
}

String newsImagePath(int index) {
  return "/news" + String(index) + ".rgb565";
}

String newsArticlePath(int index) {
  return "/article" + String(index) + ".json";
}

bool loadNewsArticle(int index, String& title, String& summary, String& source) {
  if (!newsCardReady || index < 0 || index >= feed.articleCount) return false;
  File article = SD.open(newsArticlePath(index), FILE_READ);
  if (!article) return false;
  String json = article.readString();
  article.close();
  JsonDocument stored;
  if (deserializeJson(stored, json)) return false;
  title = stored["title"] | "";
  summary = stored["summary"] | "";
  source = stored["source"] | "News";
  return title.length() > 0;
}

void cleanupNewsCache() {
  if (!newsCardReady) return;
  time_t now = time(nullptr);
  if (now < 1700000000) return; // Wait until the clock has been synchronised.
  for (int i = 0; i < MAX_NEWS_ARTICLES; i++) {
    String path = newsArticlePath(i);
    File article = SD.open(path, FILE_READ);
    if (!article) {
      SD.remove(newsImagePath(i));
      continue;
    }
    String json = article.readString();
    article.close();
    JsonDocument stored;
    uint32_t savedAt = 0;
    if (!deserializeJson(stored, json)) savedAt = stored["saved_at"] | 0UL;
    if (savedAt && (uint32_t)now > savedAt && (uint32_t)now - savedAt > NEWS_CACHE_MAX_AGE_SEC) {
      SD.remove(path);
      SD.remove(newsImagePath(i));
    }
  }
}

bool saveDataCache(const char* path, JsonDocument& doc) {
  if (!newsCardReady) return false;
  time_t now = time(nullptr);
  if (now < 1700000000) return false;
  doc["cached_at"] = (uint32_t)now;
  SD.remove(path);
  File file = SD.open(path, FILE_WRITE);
  if (!file) return false;
  serializeJson(doc, file);
  file.close();
  return true;
}

bool loadDataCache(const char* path, JsonDocument& doc, uint32_t maxAge) {
  if (!newsCardReady) return false;
  File file = SD.open(path, FILE_READ);
  if (!file) return false;
  String json = file.readString();
  file.close();
  if (deserializeJson(doc, json)) { SD.remove(path); return false; }
  time_t now = time(nullptr);
  uint32_t cachedAt = doc["cached_at"] | 0UL;
  if (now < 1700000000 || !cachedAt || (uint32_t)now < cachedAt || (uint32_t)now - cachedAt > maxAge) {
    SD.remove(path);
    return false;
  }
  return true;
}

bool loadNewsImage(int index) {
  if (!newsCardReady || index < 0 || index >= feed.articleCount) return false;
  File image = SD.open(newsImagePath(index), FILE_READ);
  if (!image || image.size() != sizeof(newsImageBuffer)) {
    if (image) image.close();
    return false;
  }
  size_t read = image.read((uint8_t*)newsImageBuffer, sizeof(newsImageBuffer));
  image.close();
  return read == sizeof(newsImageBuffer);
}

String assetPath(const String& name) {
  if (name == "sun") return "/sun.rgb565";
  if (name.startsWith("moon") || name.startsWith("weather")) {
    int split = name.startsWith("moon") ? 4 : 7;
    int index = name.substring(split).toInt();
    int maxIndex = name.startsWith("moon") ? 7 : 7;
    if (index >= 0 && index <= maxIndex) return "/" + name + ".rgb565";
  }
  return "";
}

bool loadAssetImage(const String& name) {
  String path = assetPath(name);
  if (!newsCardReady || !path.length()) return false;
  File image = SD.open(path, FILE_READ);
  if (!image) {
    if (image) image.close();
    return false;
  }
  size_t pixels = 0;
  while (image.available()) {
    int count = image.read();
    int low = image.read();
    int high = image.read();
    if (count <= 0 || low < 0 || high < 0 || pixels + (size_t)count > ASSET_IMAGE_PIXELS) {
      image.close();
      return false;
    }
    uint16_t color = ((uint16_t)high << 8) | (uint8_t)low;
    for (int i = 0; i < count; i++) assetImageBuffer[pixels++] = color;
  }
  image.close();
  return pixels == ASSET_IMAGE_PIXELS;
}

bool decodeAssetRleToFile(const String& encoded, File& file) {
  uint8_t record[3];
  size_t recordByte = 0;
  size_t pixels = 0;
  int value = 0, bits = -8;
  for (size_t i = 0; i < encoded.length(); i++) {
    int decoded = base64Value(encoded[i]);
    if (decoded < 0) continue;
    value = (value << 6) | decoded;
    bits += 6;
    if (bits >= 0) {
      record[recordByte++] = (uint8_t)((value >> bits) & 0xFF);
      bits -= 8;
      if (recordByte == sizeof(record)) {
        if (record[0] == 0 || pixels + record[0] > ASSET_IMAGE_PIXELS || file.write(record, sizeof(record)) != sizeof(record)) return false;
        pixels += record[0];
        recordByte = 0;
      }
    }
  }
  return recordByte == 0 && pixels == ASSET_IMAGE_PIXELS;
}

int drawWrappedText(const String& text, int x, int y, int width, int maxLines, uint16_t color, uint16_t background = C_BG) {
  String line;
  int lines = 0;
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(color, background);
  int start = 0;
  while (start < text.length() && lines < maxLines) {
    while (start < text.length() && text[start] == ' ') start++;
    int end = text.indexOf(' ', start);
    if (end < 0) end = text.length();
    String word = text.substring(start, end);
    String candidate = line.length() ? line + " " + word : word;
    if (!line.length() || tft.textWidth(candidate, 1) <= width) {
      line = candidate;
    } else {
      tft.drawString(line, x, y + lines * 12);
      lines++;
      line = word;
    }
    start = end + 1;
  }
  if (line.length() && lines < maxLines) {
    tft.drawString(fitText(line, width, 1), x, y + lines * 12);
    lines++;
  }
  return lines;
}

void drawNewsPage() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 320, 26, C_PANEL);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(C_ACC, C_PANEL);
  tft.drawString("Top Stories", 8, 5);

  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[8];
  if (t < 1700000000) strcpy(buf, "--:--");
  else strftime(buf, sizeof(buf), "%H:%M", &tm);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString(buf, 312, 5);

  if (feed.articleCount) {
    int articleIndex = feed.articleCount == 1 ? 0 :
      (int)(((millis() - feed.articleAt) / 30000UL) % feed.articleCount);
    lastNewsArticleSlot = (millis() - feed.articleAt) / 30000UL;
    NewsArticle& article = feed.articles[articleIndex];
    String title, summary, source;
    if (!loadNewsArticle(articleIndex, title, summary, source)) {
      title = articleIndex < feed.count ? feed.headlines[articleIndex] : "Top Story";
      summary = newsCardReady ? "Article details are being cached." : "Insert a FAT32 SD card to cache article details and images.";
      source = "Top Stories";
    }
    // Keep the complete article together as a single card so the image,
    // headline and summary have a clear visual boundary on the small screen.
    tft.fillRoundRect(6, 30, 308, 185, 8, C_PANEL);
    tft.drawRoundRect(6, 30, 308, 185, 8, C_LINE);
    if (article.imageReady && loadNewsImage(articleIndex)) {
      // RGB565 values arrive in the ESP32's native uint16_t representation,
      // while the ILI9341 expects each pixel's high byte first on SPI.
      // TFT_eSPI's swap path handles that boundary; without it, adjacent
      // pixels become the coloured-noise pattern seen on the display.
      bool previousSwap = tft.getSwapBytes();
      tft.setSwapBytes(true);
      tft.pushImage(210, 40, NEWS_IMAGE_W, NEWS_IMAGE_H, newsImageBuffer);
      tft.setSwapBytes(previousSwap);
    } else tft.fillRect(210, 40, NEWS_IMAGE_W, NEWS_IMAGE_H, C_PANEL);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(C_TEXT, C_PANEL);
    drawWrappedText(title, 14, 40, 190, 3, C_TEXT, C_PANEL);
    tft.setTextFont(1);
    tft.setTextColor(C_ACC, C_PANEL);
    tft.drawString(fitText(source, 190, 1), 14, 94);
    drawWrappedText(summary, 14, 114, 292, 7, C_TEXT, C_PANEL);
    tft.setTextColor(C_DIM, C_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("ARTICLE " + String(articleIndex + 1) + "/" + String(feed.articleCount), 8, 222);
  } else {
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
  }

  tft.setTextFont(1);
  tft.setTextColor(C_DIM, C_BG);
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

  if (!drawWeatherAsset(feed.code, 15, 36)) drawWeatherIcon(feed.code, 42, 62, 2);
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
    if (!drawWeatherAsset(feed.forecastCode[i], x + 9, 143)) drawWeatherIcon(feed.forecastCode[i], x + 36, 161, 1);

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

void drawPageHeader(const String& title, const String& detail) {
  tft.fillRect(0,0,320,30,C_PANEL);
  tft.fillRect(0,28,54,2,C_ACC);
  tft.setTextDatum(TL_DATUM); tft.setTextFont(2); tft.setTextColor(C_ACC,C_PANEL);
  tft.drawString(title,10,7);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(C_DIM,C_PANEL);
  tft.drawString(fitText(detail,190,2),310,7);
}

void drawMetricCard(int x,int y,int w,int h,const String& label,const String& value,const String& foot,uint16_t accent) {
  tft.fillRoundRect(x,y,w,h,8,C_PANEL);
  tft.fillRoundRect(x,y,3,h,2,accent);
  tft.setTextDatum(TL_DATUM); tft.setTextFont(1); tft.setTextColor(C_DIM,C_PANEL); tft.drawString(label,x+10,y+7);
  tft.setTextFont(4); tft.setTextColor(C_TEXT,C_PANEL); tft.drawString(fitText(value,w-18,4),x+10,y+20);
  if(foot.length()){tft.setTextFont(1);tft.setTextColor(C_DIM,C_PANEL);tft.drawString(fitText(foot,w-18,1),x+10,y+h-12);}
}

int nextTideEvent(bool high) {
  int best = -1;
  for (int i = 0; i < tide.futureEventCount; i++) {
    if (tide.futureEventMinutes[i] < 0) continue;
    if ((tide.futureEventKind[i] == "H") != high) continue;
    if (best < 0 || tide.futureEventMinutes[i] < tide.futureEventMinutes[best]) best = i;
  }
  return best;
}

void drawTideCard(int x,int y,int w,int h,const String& label,const String& value,const String& foot,uint16_t accent) {
  tft.fillRoundRect(x,y,w,h,7,C_PANEL);
  tft.fillRoundRect(x,y,3,h,2,accent);
  tft.setTextDatum(TL_DATUM); tft.setTextFont(1); tft.setTextColor(C_DIM,C_PANEL); tft.drawString(label,x+9,y+6);
  tft.setTextFont(4); tft.setTextColor(C_TEXT,C_PANEL); tft.drawString(fitText(value,w-16,4),x+9,y+17);
  if(foot.length()){tft.setTextFont(1); tft.setTextColor(C_DIM,C_PANEL); tft.drawString(fitText(foot,w-16,1),x+9,y+h-11);}
}

void drawTidePage() {
  tft.fillScreen(C_BG);
  drawPageHeader("TIDE", (tide.station.length()?tide.station:cfg.tidePort)+"  /  METRES");
  if(!tide.valid){tft.setTextDatum(MC_DATUM);tft.setTextFont(2);tft.setTextColor(C_DIM,C_BG);tft.drawString("Waiting for tide observations",160,118);return;}

  int nextHigh=nextTideEvent(true), nextLow=nextTideEvent(false);
  float current=tide.hasCurrent?tide.current:tide.latest;
  drawTideCard(8,36,98,60,"CURRENT HEIGHT",String(current,2),tide.hasCurrent?"predicted now":tide.latestTime+" local",C_ACC);
  drawTideCard(111,36,98,60,"NEXT HIGH",nextHigh<0?"--:--":tide.futureEventTime[nextHigh],nextHigh<0?"No forecast":String(tide.futureEventHeight[nextHigh],2)+" m",C_WARN);
  drawTideCard(214,36,98,60,"NEXT LOW",nextLow<0?"--:--":tide.futureEventTime[nextLow],nextLow<0?"No forecast":String(tide.futureEventHeight[nextLow],2)+" m",C_OK);

  tft.fillRoundRect(8,100,304,100,8,C_PANEL);
  tft.setTextDatum(TL_DATUM);tft.setTextFont(1);tft.setTextColor(C_DIM,C_PANEL);tft.drawString("PAST 12 HOURS",17,106);
  tft.setTextDatum(TR_DATUM);tft.drawString("NEXT 12 HOURS",303,106);

  const int left=34,right=308,top=124,bottom=182,center=(left+right)/2,window=720;
  auto xAt=[&](int mins){return (int)(center + (long)mins*(right-center)/window);};

  float lo=10000,hi=-10000; bool any=false;
  for(int i=0;i<tide.count;i++){
    int m=-((int)tide.minutesAgo[i]+tide.latestAge);
    if(m<-window||m>0) continue;
    lo=min(lo,tide.heights[i]);hi=max(hi,tide.heights[i]);any=true;
  }
  for(int i=0;i<tide.futureCount;i++){
    int m=tide.futureMinutes[i];
    if(m<-window||m>window) continue;
    lo=min(lo,tide.futureHeights[i]);hi=max(hi,tide.futureHeights[i]);any=true;
  }
  lo=min(lo,current);hi=max(hi,current);any=true;
  if(!any){lo=tide.latest-0.5f;hi=tide.latest+0.5f;}
  float pad=max(0.15f,(hi-lo)*0.10f);lo-=pad;hi+=pad;
  if(hi-lo<0.001f){lo-=0.5f;hi+=0.5f;}
  auto yAt=[&](float v){return bottom-(int)((v-lo)*(bottom-top)/(hi-lo));};

  for(int g=0;g<3;g++){int gy=top+g*(bottom-top)/2;tft.drawFastHLine(left,gy,right-left,C_LINE);}
  tft.setTextDatum(TR_DATUM);tft.setTextColor(C_DIM,C_PANEL);
  tft.drawString(String(hi,1),left-4,top-7);
  tft.drawString(String(lo,1),left-4,bottom-7);

  bool have=false;int px=0,py=0;
  for(int i=0;i<tide.count;i++){
    int m=-((int)tide.minutesAgo[i]+tide.latestAge);
    if(m<-window) continue;
    if(m>0) break;
    int x=xAt(m),y=yAt(tide.heights[i]);
    if(have)tft.drawLine(px,py,x,y,C_ACC);
    px=x;py=y;have=true;
  }
  for(int i=0;i<tide.futureCount;i++){
    int m=tide.futureMinutes[i];
    if(m<-window) continue;
    if(m>window) break;
    int x=xAt(m),y=yAt(tide.futureHeights[i]);
    if(have)tft.drawLine(px,py,x,y,C_OK);
    px=x;py=y;have=true;
  }

  for(int i=0;i<tide.eventCount;i++){
    int m=-((int)tide.eventMinutesAgo[i]+tide.latestAge);
    if(m<-window||m>0) continue;
    tft.fillCircle(xAt(m),yAt(tide.eventHeight[i]),2,tide.eventKind[i]=="H"?C_WARN:C_ACC);
  }
  for(int i=0;i<tide.futureEventCount;i++){
    int m=tide.futureEventMinutes[i];
    if(m<0||m>window) continue;
    int x=xAt(m),y=yAt(tide.futureEventHeight[i]);
    bool high=tide.futureEventKind[i]=="H";
    uint16_t col=high?C_WARN:C_ACC;
    tft.fillCircle(x,y,3,col);
    int ly=high?y-10:y+10;
    if(ly<top+2) ly=y+10;
    if(ly>bottom-2) ly=y-10;
    tft.setTextDatum(MC_DATUM);tft.setTextFont(1);tft.setTextColor(col,C_PANEL);
    tft.drawString(high?"H":"L",x,ly);
  }

  tft.drawFastVLine(center,top,bottom-top,C_LINE);
  int cy=yAt(current);
  tft.fillCircle(center,cy,4,C_TEXT);
  tft.drawCircle(center,cy,6,C_ACC);

  tft.setTextDatum(TL_DATUM);tft.setTextFont(1);tft.setTextColor(C_DIM,C_PANEL);
  tft.drawString("-12h",26,188);
  tft.setTextDatum(TC_DATUM);tft.drawString("NOW",center,188);
  tft.setTextDatum(TR_DATUM);tft.drawString("+12h",right,188);

  tft.fillRoundRect(8,205,304,26,7,C_PANEL);
  tft.setTextDatum(TL_DATUM);tft.setTextFont(1);tft.setTextColor(C_DIM,C_PANEL);
  String footer="GAUGE "+tide.latestTime+"  "+String(tide.latest,1)+"m";
  footer+=tide.latestAge>0?"    UPDATED "+String(tide.latestAge)+"m AGO":"    LIVE";
  tft.drawString(fitText(footer,286,1),17,214);
}

void drawMarinePage() {
  tft.fillScreen(C_BG);drawPageHeader("MARINE",fitText(marine.station.length()?marine.station:cfg.tidePort,190,2));
  if(!marine.valid){tft.setTextDatum(MC_DATUM);tft.setTextFont(2);tft.setTextColor(C_DIM,C_BG);tft.drawString("Waiting for marine conditions",160,118);return;}
  String direction=marine.windDirection<0?"":"  "+String((int)marine.windDirection)+" deg";
  drawMetricCard(8,38,148,76,"WIND",marine.wind<0?"--":String(marine.wind,1)+" "+marine.windUnit+direction,marine.windGust<0?"":"Gusts "+String(marine.windGust,1)+" "+marine.windUnit,C_ACC);
  drawMetricCard(164,38,148,76,"WAVES",marine.waveHeight<0?"--":String(marine.waveHeight,1)+" m",marine.wavePeriod<0?"":"Period  "+String(marine.wavePeriod,0)+" sec",C_OK);
  drawMetricCard(8,122,148,76,"SEA TEMPERATURE",marine.seaTemp<-90?"--":String(marine.seaTemp,1)+" C","Surface",C_WARN);
  drawMetricCard(164,122,148,76,"CURRENT",marine.currentSpeed<0?"--":String(marine.currentSpeed,1)+" kn",marine.currentDirection<0?"":"Direction  "+String((int)marine.currentDirection)+" deg",C_ACC);
  tft.fillRoundRect(8,206,304,26,7,C_PANEL);tft.setTextDatum(TL_DATUM);tft.setTextFont(1);tft.setTextColor(C_DIM,C_PANEL);
  String astronomy="MOON  "+(marine.moon.length()?marine.moon+"  "+String(marine.moonIllumination)+"%":"--")+"     SUN  "+marine.sunrise+" / "+marine.sunset;
  tft.drawString(fitText(astronomy,286,1),17,215);
}

int clockMinutes(const String& value) {
  if (value.length() < 5 || value[2] != ':') return -1;
  int hour = value.substring(0, 2).toInt();
  int minute = value.substring(3, 5).toInt();
  return (hour >= 0 && hour < 24 && minute >= 0 && minute < 60) ? hour * 60 + minute : -1;
}

void drawMoonGraphic(int cx, int cy, int radius, const String& phase, int illumination) {
  tft.fillCircle(cx, cy, radius, C_LINE);
  if (illumination < 5) return;
  if (illumination > 95) { tft.fillCircle(cx, cy, radius, C_TEXT); return; }
  bool waxing = phase.startsWith("Waxing") || phase == "First quarter";
  if (illumination >= 45 && illumination <= 55) {
    tft.fillRect(waxing ? cx : cx - radius, cy - radius, radius, radius * 2, C_TEXT);
    return;
  }
  tft.fillCircle(cx, cy, radius, C_TEXT);
  int offset = max(2, (int)(radius * (1.0f - illumination / 50.0f)));
  if (illumination > 50) offset = max(2, (int)(radius * ((illumination - 50.0f) / 50.0f)));
  tft.fillCircle(cx + (waxing ? offset : -offset), cy, radius, C_LINE);
}

int moonAssetIndex(const String& phase) {
  if (phase == "New moon") return 0;
  if (phase == "Waxing crescent") return 1;
  if (phase == "First quarter") return 2;
  if (phase == "Waxing gibbous") return 3;
  if (phase == "Full moon") return 4;
  if (phase == "Waning gibbous") return 5;
  if (phase == "Last quarter") return 6;
  if (phase == "Waning crescent") return 7;
  return -1;
}

int weatherAssetIndex(int code) {
  if (code == 0) return 0;
  if (code == 1 || code == 2) return 1;
  if (code == 3) return 2;
  if (code == 45 || code == 48) return 3;
  if (code >= 71 && code <= 77 || code == 85 || code == 86) return 6;
  if (code >= 95) return 7;
  if (code >= 61 && code <= 67 || code >= 80 && code <= 82) return 5;
  return 4;
}

bool drawWeatherAsset(int code, int x, int y) {
  String name = "weather" + String(weatherAssetIndex(code));
  if (!loadAssetImage(name)) return false;
  bool previousSwap = tft.getSwapBytes();
  tft.setSwapBytes(true);
  tft.pushImage(x, y, ASSET_IMAGE_W, ASSET_IMAGE_H, assetImageBuffer, ASSET_TRANSPARENT);
  tft.setSwapBytes(previousSwap);
  return true;
}

void drawSunMoonPage() {
  tft.fillScreen(C_BG); drawPageHeader("SUN & MOON", marine.station.length() ? marine.station : cfg.city);
  if (!marine.valid) {
    tft.setTextDatum(MC_DATUM); tft.setTextFont(2); tft.setTextColor(C_DIM, C_BG);
    tft.drawString("Waiting for astronomy data", 160, 118);
    return;
  }

  int sunrise = clockMinutes(marine.sunrise);
  int sunset = clockMinutes(marine.sunset);
  time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  int current = local.tm_hour * 60 + local.tm_min;
  bool daylight = sunrise >= 0 && sunset >= 0 && current >= sunrise && current < sunset;
  int daylightMinutes = sunrise >= 0 && sunset >= 0 ? sunset - sunrise : -1;

  tft.fillRoundRect(8, 38, 148, 80, 8, C_PANEL);
  tft.fillRoundRect(164, 38, 148, 80, 8, C_PANEL);
  tft.setTextDatum(TL_DATUM); tft.setTextFont(1); tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString("SUN", 18, 46); tft.drawString("MOON", 174, 46);
  drawSunIcon(42, 78, 10);
  if (!loadAssetImage("moon" + String(moonAssetIndex(marine.moon)))) drawMoonGraphic(198, 78, 18, marine.moon, marine.moonIllumination);
  else {
    bool previousSwap = tft.getSwapBytes();
    tft.setSwapBytes(true);
    tft.pushImage(171, 52, ASSET_IMAGE_W, ASSET_IMAGE_H, assetImageBuffer, ASSET_TRANSPARENT);
    tft.setSwapBytes(previousSwap);
  }
  tft.setTextColor(C_TEXT, C_PANEL); tft.setTextFont(2);
  tft.drawString("RISE  "+(marine.sunrise.length()?marine.sunrise:"--:--"), 68, 66);
  tft.drawString("SET   "+(marine.sunset.length()?marine.sunset:"--:--"), 68, 88);
  tft.drawString(fitText(marine.moon.length()?marine.moon:"Unknown", 86, 2), 222, 68);
  tft.setTextFont(1); tft.setTextColor(C_ACC, C_PANEL);
  tft.drawString(fitText(marine.moonIllumination >= 0 ? String(marine.moonIllumination)+"% illuminated" : "-- illumination", 86, 1), 222, 94);

  tft.fillRoundRect(8, 126, 304, 72, 8, C_PANEL);
  tft.setTextDatum(TL_DATUM); tft.setTextFont(1); tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString("TODAY", 17, 134); tft.drawString("DAYLIGHT", 17, 158); tft.drawString("STATUS", 17, 180);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(C_TEXT, C_PANEL);
  String duration = daylightMinutes >= 0 ? String(daylightMinutes / 60) + "h " + String(daylightMinutes % 60) + "m" : "--";
  tft.drawString(duration, 303, 158);
  tft.setTextColor(daylight ? C_SUN : C_DIM, C_PANEL);
  tft.drawString(daylight ? "DAYLIGHT" : "NIGHT", 303, 180);

  tft.fillRoundRect(8, 206, 304, 26, 7, C_PANEL);
  tft.setTextDatum(TL_DATUM); tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString("LOCAL TIME  "+String(local.tm_hour < 10 ? "0" : "")+String(local.tm_hour)+":"+String(local.tm_min < 10 ? "0" : "")+String(local.tm_min), 17, 215);
}

void drawPcPage() {
  tft.fillScreen(C_BG);drawPageHeader("PC DETAILS",stats.host.length()?stats.host:"AGENT OFFLINE");
  drawMetricCard(8,38,148,76,"PROCESSOR",stats.cpu<0?"--":String(stats.cpu,0)+"%",stats.cpuTemp<0?"Temperature --":fmtTemp(stats.cpuTemp),C_ACC);
  drawMetricCard(164,38,148,76,"MEMORY",stats.ram<0?"--":String(stats.ram,0)+"%",stats.ramUsed<0?"--":"Used "+String(stats.ramUsed,1)+" / "+String(stats.ramTotal,1)+" GB",C_OK);
  drawMetricCard(8,122,148,76,"STORAGE",stats.disk<0?"--":String(stats.disk,0)+"%",stats.diskUsed<0?"--":"Used "+String(stats.diskUsed,1)+" / "+String(stats.diskTotal,1)+" GB",C_WARN);
  drawMetricCard(164,122,148,76,"NETWORK","↓ "+String(stats.netDown,1)+"  ↑ "+String(stats.netUp,1),"Mbps  /  current throughput",C_ACC);
  tft.fillRoundRect(8,206,304,26,7,C_PANEL);tft.setTextDatum(TL_DATUM);tft.setTextFont(1);tft.setTextColor(C_DIM,C_PANEL);
  tft.drawString("HOST UPTIME   "+fmtUptime(stats.hostUptime),17,215);
}

void updatePcCard(int x,int y,int w,int h,const String& value,const String& foot){
  tft.fillRect(x+8,y+20,w-16,h-24,C_PANEL);tft.setTextDatum(TL_DATUM);tft.setTextFont(4);tft.setTextColor(C_TEXT,C_PANEL);tft.drawString(fitText(value,w-18,4),x+10,y+23);
  if(foot.length()){tft.setTextFont(1);tft.setTextColor(C_DIM,C_PANEL);tft.drawString(fitText(foot,w-18,1),x+10,y+h-15);}
}
void drawPcMetrics(){
  updatePcCard(8,38,148,76,stats.cpu<0?"--":String(stats.cpu,0)+"%",stats.cpuTemp<0?"Temperature --":fmtTemp(stats.cpuTemp));
  updatePcCard(164,38,148,76,stats.ram<0?"--":String(stats.ram,0)+"%",stats.ramUsed<0?"--":"Used "+String(stats.ramUsed,1)+" / "+String(stats.ramTotal,1)+" GB");
  updatePcCard(8,122,148,76,stats.disk<0?"--":String(stats.disk,0)+"%",stats.diskUsed<0?"--":"Used "+String(stats.diskUsed,1)+" / "+String(stats.diskTotal,1)+" GB");
  updatePcCard(164,122,148,76,"↓ "+String(stats.netDown,1)+"  ↑ "+String(stats.netUp,1),"Mbps  /  current throughput");
  tft.fillRect(14,211,288,16,C_PANEL);tft.setTextDatum(TL_DATUM);tft.setTextFont(1);tft.setTextColor(C_DIM,C_PANEL);tft.drawString("HOST UPTIME   "+fmtUptime(stats.hostUptime),17,215);
}

void drawPage(bool full) {
  if (page == 0) drawDashboard();
  else if (page == 1) drawNewsPage();
  else if (page == 2) drawForecastPage();
  else if (page == 3) drawTidePage();
  else if (page == 4) drawMarinePage();
  else if (page == 5) drawPcPage();
  else if (page == 6) drawSunMoonPage();
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
  doc["brightness"] = appliedBrightness; doc["light"] = lightRaw; doc["sleeping"] = displaySleeping;
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
  doc["tidePort"] = cfg.tidePort; doc["tideStation"] = cfg.tideStation;
  doc["autoBrightness"] = cfg.autoBrightness; doc["sleepNoAgent"] = cfg.sleepNoAgent;
  doc["autoRotate"] = cfg.autoRotate; doc["rotateMinutes"] = cfg.rotateMinutes;
  doc["colorBg"] = cfg.colorBg; doc["colorPanel"] = cfg.colorPanel; doc["colorLine"] = cfg.colorLine;
  doc["colorText"] = cfg.colorText; doc["colorDim"] = cfg.colorDim; doc["colorAccent"] = cfg.colorAccent;
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
  if (doc["tidePort"].is<const char*>()) cfg.tidePort = doc["tidePort"].as<String>();
  if (doc["tideStation"].is<const char*>()) cfg.tideStation = doc["tideStation"].as<String>();
  if (doc["brightness"].is<int>()) cfg.brightness = constrain(doc["brightness"].as<int>(), 10, 255);
  if (doc["invert"].is<bool>()) cfg.invert = doc["invert"].as<bool>();
  if (doc["rotation"].is<int>()) cfg.rotation = doc["rotation"].as<int>() == 3 ? 3 : 1;
  if (doc["autoBrightness"].is<bool>()) cfg.autoBrightness = doc["autoBrightness"].as<bool>();
  if (doc["sleepNoAgent"].is<bool>()) cfg.sleepNoAgent = doc["sleepNoAgent"].as<bool>();
  if (doc["autoRotate"].is<bool>()) cfg.autoRotate = doc["autoRotate"].as<bool>();
  if (doc["rotateMinutes"].is<int>()) { int m=doc["rotateMinutes"].as<int>(); if(m==1||m==2||m==5||m==10||m==15||m==30) cfg.rotateMinutes=m; }
  if (doc["colorBg"].is<uint32_t>()) cfg.colorBg=doc["colorBg"].as<uint32_t>();
  if (doc["colorPanel"].is<uint32_t>()) cfg.colorPanel=doc["colorPanel"].as<uint32_t>();
  if (doc["colorLine"].is<uint32_t>()) cfg.colorLine=doc["colorLine"].as<uint32_t>();
  if (doc["colorText"].is<uint32_t>()) cfg.colorText=doc["colorText"].as<uint32_t>();
  if (doc["colorDim"].is<uint32_t>()) cfg.colorDim=doc["colorDim"].as<uint32_t>();
  if (doc["colorAccent"].is<uint32_t>()) cfg.colorAccent=doc["colorAccent"].as<uint32_t>();

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
  stats.disk = doc["disk"] | -1.0f; stats.diskUsed=doc["disk_used"] | -1.0f; stats.diskTotal=doc["disk_total"] | -1.0f;
  stats.netDown=doc["net_down_mbps"] | 0.0f; stats.netUp=doc["net_up_mbps"] | 0.0f;
  stats.hostUptime=doc["host_uptime"] | 0UL;
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
  cleanupNewsCache();
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
    if (feed.count >= MAX_HEADLINES) break;
    String h = v.as<String>();
    h.trim();
    if (h.length()) feed.headlines[feed.count++] = h;
  }
  feed.articleCount = 0;
  for (JsonObject article : doc["articles"].as<JsonArray>()) {
    if (feed.articleCount >= MAX_NEWS_ARTICLES) break;
    int i = feed.articleCount++;
    if (newsCardReady) {
      JsonDocument stored;
      stored["title"] = article["title"] | "";
      stored["summary"] = article["summary"] | "";
      stored["source"] = article["source"] | feed.source;
      stored["link"] = article["link"] | "";
      time_t savedAt = time(nullptr);
      if (savedAt >= 1700000000) stored["saved_at"] = (uint32_t)savedAt;
      String path = newsArticlePath(i);
      SD.remove(path);
      File saved = SD.open(path, FILE_WRITE);
      if (saved) { serializeJson(stored, saved); saved.close(); }
    }
    feed.articles[i].imageReady = false;
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
  feed.articleAt = feed.at;
  lastNewsArticleSlot = 0;
  lastNewsImageMs = millis();
  buildTicker();
  feedDirty = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleNewsImage() {
  int index = server.arg("i").toInt();
  if (index < 0 || index >= feed.articleCount || !newsCardReady) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  if (!decodeNewsImage(server.arg("plain"), newsImageBuffer)) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  String path = newsImagePath(index);
  SD.remove(path);
  File image = SD.open(path, FILE_WRITE);
  if (!image || image.write((const uint8_t*)newsImageBuffer, sizeof(newsImageBuffer)) != sizeof(newsImageBuffer)) {
    if (image) image.close();
    server.send(500, "application/json", "{\"ok\":false}");
    return;
  }
  image.close();
  feed.articles[index].imageReady = true;
  lastNewsImageMs = millis();
  feedDirty = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleAssetImage() {
  String name = server.arg("name");
  String path = assetPath(name);
  if (!newsCardReady || !path.length()) {
    server.send(400, "application/json", "{\"ok\":false}"); return;
  }
  SD.remove(path);
  File image = SD.open(path, FILE_WRITE);
  bool valid = image && decodeAssetRleToFile(server.arg("plain"), image);
  if (image) image.close();
  if (!valid) {
    SD.remove(path);
    server.send(400, "application/json", "{\"ok\":false}"); return;
  }
  if (!loadAssetImage(name)) {
    SD.remove(path);
    server.send(500, "application/json", "{\"ok\":false}"); return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSdGet() {
  JsonDocument doc;
  doc["ready"] = newsCardReady;
  doc["totalMb"] = newsCardReady ? (uint32_t)(SD.totalBytes() / (1024 * 1024)) : 0;
  doc["usedMb"] = newsCardReady ? (uint32_t)(SD.usedBytes() / (1024 * 1024)) : 0;
  int assets = 0;
  if (newsCardReady) {
    if (SD.exists("/sun.rgb565")) assets++;
    for (int i = 0; i < 8; i++) {
      if (SD.exists(("/moon" + String(i) + ".rgb565").c_str())) assets++;
      if (SD.exists(("/weather" + String(i) + ".rgb565").c_str())) assets++;
    }
  }
  doc["assetCount"] = assets;
  String out; serializeJson(doc, out); server.send(200, "application/json", out);
}

void handleSdPost() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) || !doc["format"].as<bool>()) {
    server.send(400, "application/json", "{\"ok\":false}"); return;
  }
  SD.end();
  delay(100);
  newsCardReady = SD.begin(5, newsCardSPI, 4000000, "/sd", 5, true);
  if (newsCardReady) cleanupNewsCache();
  server.send(newsCardReady ? 200 : 500, "application/json", newsCardReady ? "{\"ok\":true}" : "{\"ok\":false}");
}

void applyTideDocument(JsonDocument& doc) {
  tide.station=doc["station"]|cfg.tidePort; tide.latestTime=doc["latest_time"]|""; tide.latestAge=doc["latest_age"]|0; tide.count=0;
  for(JsonArray row:doc["points"].as<JsonArray>()){if(tide.count>=MAX_TIDE_POINTS)break;int i=tide.count++; tide.minutesAgo[i]=row[0]|0; tide.heights[i]=row[1]|0.0f;}
  tide.eventCount=0; for(JsonArray row:doc["events"].as<JsonArray>()){if(tide.eventCount>=MAX_TIDE_EVENTS)break;int i=tide.eventCount++;tide.eventMinutesAgo[i]=row[0]|0;tide.eventHeight[i]=row[1]|0.0f;tide.eventKind[i]=row[2]|"";tide.eventTime[i]=row[3]|"";}
  tide.futureCount=0; for(JsonArray row:doc["future"].as<JsonArray>()){if(tide.futureCount>=MAX_TIDE_FUTURE)break;int i=tide.futureCount++;tide.futureMinutes[i]=row[0]|0;tide.futureHeights[i]=row[1]|0.0f;}
  tide.futureEventCount=0; for(JsonArray row:doc["future_events"].as<JsonArray>()){if(tide.futureEventCount>=MAX_TIDE_FUTURE_EVENTS)break;int i=tide.futureEventCount++;tide.futureEventMinutes[i]=row[0]|0;tide.futureEventHeight[i]=row[1]|0.0f;tide.futureEventKind[i]=row[2]|"";tide.futureEventTime[i]=row[3]|"";}
  tide.latest=tide.count?tide.heights[tide.count-1]:0;tide.valid=tide.count>0;tide.at=millis();
  tide.hasCurrent=!doc["current"].isNull();tide.current=tide.hasCurrent?doc["current"].as<float>():tide.latest;
}

void handleTide() {
  JsonDocument doc; if(deserializeJson(doc,server.arg("plain"))){server.send(400,"application/json","{\"ok\":false}");return;}
  applyTideDocument(doc); saveDataCache("/tide.json", doc);
  server.send(200,"application/json","{\"ok\":true}");
}

void applyMarineDocument(JsonDocument& doc) {
  marine.station=doc["station"]|cfg.tidePort;marine.wind=doc["wind"]|-1.0f;marine.windDirection=doc["wind_direction"]|-1.0f;marine.windGust=doc["wind_gust"]|-1.0f;marine.windUnit=doc["wind_unit"]|"";
  marine.waveHeight=doc["wave_height"]|-1.0f;marine.wavePeriod=doc["wave_period"]|-1.0f;marine.seaTemp=doc["sea_temp"]|-100.0f;marine.currentSpeed=doc["current_speed"]|-1.0f;marine.currentDirection=doc["current_direction"]|-1.0f;
  marine.sunrise=doc["sunrise"]|"";marine.sunset=doc["sunset"]|"";marine.moon=doc["moon"]|"";marine.moonIllumination=doc["moon_illumination"]|-1;
  marine.valid=true;marine.at=millis();
}

void handleMarine() {
  JsonDocument doc; if(deserializeJson(doc,server.arg("plain"))){server.send(400,"application/json","{\"ok\":false}");return;}
  applyMarineDocument(doc); saveDataCache("/marine.json", doc);
  server.send(200,"application/json","{\"ok\":true}");
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
  server.on("/api/newsimage", HTTP_POST, handleNewsImage);
  server.on("/api/asset", HTTP_POST, handleAssetImage);
  server.on("/api/sd", HTTP_GET, handleSdGet);
  server.on("/api/sd", HTTP_POST, handleSdPost);
  server.on("/api/tide", HTTP_POST, handleTide);
  server.on("/api/marine", HTTP_POST, handleMarine);
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
  page = (page + 1) % 8;
  drawPage(true);
}

void updateDisplay() {
  uint32_t now = millis();
  updateBacklight();
  if (cfg.autoRotate && page != 7 && now - lastPageChange >= (uint32_t)cfg.rotateMinutes * 60000UL) {
    page = (page + 1) % 7; lastPageChange = now; drawPage(true);
  }
  if (displaySleeping) return;
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
  } else if (page == 1) {
    if (feedDirty && now - lastNewsImageMs >= 700) { feedDirty=false; drawNewsPage(); }
    else if (feed.articleCount > 1 && (now - feed.articleAt) / 30000UL != lastNewsArticleSlot) drawNewsPage();
  } else if (page == 2) {
    if (feedDirty) { feedDirty=false; drawForecastPage(); }
  } else if (page == 3) {
    if (now-lastTideMs>=60000 && tide.valid) {lastTideMs=now;drawTidePage();}
  } else if (page == 4) {
    if (now-lastMarineMs>=60000 && marine.valid) {lastMarineMs=now;drawMarinePage();}
  } else if (page == 5) {
    if (now-lastPcMs>=3000) {lastPcMs=now;drawPcMetrics();}
  } else if (page == 6) {
    if (now-lastMarineMs>=60000 && marine.valid) {lastMarineMs=now;drawSunMoonPage();}
  } else {
    if (now - lastSysMs >= 2000) { lastSysMs = now; drawSystemPage(); }
  }
}

void setup() {
  Serial.begin(115200);
  bootMs = millis();
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  pinMode(PIN_LDR, INPUT);
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

  newsCardSPI.begin(18, 19, 23, 5);
  newsCardReady = SD.begin(5, newsCardSPI);
  cleanupNewsCache();
  Serial.printf("News SD card: %s\n", newsCardReady ? "ready" : "not available");

  ticker.setColorDepth(16);
  tickerReady = ticker.createSprite(320, 45) != nullptr;
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
