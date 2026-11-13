/*
  ThermoBox v1.0 firmware  (ESP32-C3-WROOM-02, SSD1306 128x64 I2C, SHT40, LiPo 1S)

  Arduino IDE:
    Board package : "esp32" by Espressif (3.x)
    Board         : "ESP32C3 Dev Module"
    USB CDC On Boot : Enabled      (Serial over the USB-C port)
    Flash size    : 4MB
    Library       : U8g2 (by olikraus)  - Library Manager

  Controls (one side button SW1):
    press while asleep  -> wake up, show temperature
    short press         -> next screen (temperature / statistics)
    long press (0.7 s)  -> game menu
    in menu: short = next game, long = start
    in game: the button is the only control
    no presses for 10 s -> deep sleep (60 s while USB is plugged in)

  Hardware map (see schematic):
    GPIO3  BTN_WAKE   active LOW, 100k pull-up, RTC-capable -> deep sleep wake
    GPIO6  I2C_SDA    GPIO7 I2C_SCL  (pull-ups on the switched VPER rail)
    GPIO10 OLED_EN    LOW = OLED + SHT40 powered (P-MOSFET Q2), 100k pull-up = off
    GPIO1  BAT_ADC    VBAT / 2
    GPIO0  VBUS_SENSE VBUS / 2 (HIGH when USB is plugged in)
*/
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include "esp_sleep.h"

// ------------------------------------------------------------------ settings
constexpr int PIN_BTN = 3;
constexpr int PIN_SDA = 6;
constexpr int PIN_SCL = 7;
constexpr int PIN_OLED_EN = 10;
constexpr int PIN_BAT = 1;
constexpr int PIN_VBUS = 0;

constexpr uint32_t IDLE_MS = 10000;      // awake time after the last press
constexpr uint32_t IDLE_USB_MS = 60000;  // longer while USB is connected (easy upload)
constexpr uint32_t LONG_PRESS_MS = 700;
constexpr uint32_t LOG_INTERVAL_S = 600; // quiet timer wake for min/max log, 0 = off

constexpr uint8_t SHT40_ADDR = 0x44;

// Most 0.96" modules are SSD1306. If the picture is shifted by 2 px, use the SH1106 line.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
// U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);

Preferences prefs;

// survives deep sleep (lost only on power loss / reset)
RTC_DATA_ATTR float rtcMin = 1000, rtcMax = -1000;
RTC_DATA_ATTR uint32_t rtcSamples = 0;

float tempC = NAN, humid = NAN;
uint32_t lastActivity = 0;

// ------------------------------------------------------------------ power
void peripheralsOn() {
  pinMode(PIN_OLED_EN, OUTPUT);
  digitalWrite(PIN_OLED_EN, LOW);   // P-MOSFET on
  delay(5);                          // SHT40 needs 1 ms, OLED a little more
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
}

void peripheralsOff() {
  Wire.end();
  pinMode(PIN_SDA, INPUT);           // never drive pins into an unpowered chip
  pinMode(PIN_SCL, INPUT);
  pinMode(PIN_OLED_EN, INPUT);       // external 100k pull-up keeps Q2 off in sleep
}

bool usbPresent() { return digitalRead(PIN_VBUS) == HIGH; }
bool btnDown() { return digitalRead(PIN_BTN) == LOW; }

void goToSleep() {
  oled.setPowerSave(1);
  peripheralsOff();
  while (btnDown()) delay(5);        // do not wake again instantly
  delay(20);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_BTN, ESP_GPIO_WAKEUP_GPIO_LOW);
  if (LOG_INTERVAL_S) esp_sleep_enable_timer_wakeup((uint64_t)LOG_INTERVAL_S * 1000000ULL);
  Serial.flush();
  esp_deep_sleep_start();
}

// ------------------------------------------------------------------ sensors
static uint8_t crc8(const uint8_t *d, int n) {
  uint8_t c = 0xFF;
  for (int i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c & 0x80) ? (c << 1) ^ 0x31 : (c << 1);
  }
  return c;
}

bool readSHT40(float &t, float &rh) {
  Wire.beginTransmission(SHT40_ADDR);
  Wire.write(0xFD);                  // measure, high precision, no heater
  if (Wire.endTransmission() != 0) return false;
  delay(10);
  if (Wire.requestFrom(SHT40_ADDR, (uint8_t)6) != 6) return false;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  if (crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5]) return false;
  uint16_t st = (b[0] << 8) | b[1], srh = (b[3] << 8) | b[4];
  t = -45.0f + 175.0f * st / 65535.0f;
  rh = constrain(-6.0f + 125.0f * srh / 65535.0f, 0.0f, 100.0f);
  return true;
}

void sampleTemperature() {
  if (readSHT40(tempC, humid)) {
    rtcMin = min(rtcMin, tempC);
    rtcMax = max(rtcMax, tempC);
    rtcSamples++;
  } else {
    tempC = humid = NAN;
  }
}

float readVbat() {
  uint32_t s = 0;
  for (int i = 0; i < 16; i++) s += analogReadMilliVolts(PIN_BAT);
  return s / 16.0f * 2.0f / 1000.0f;  // 1M/1M divider
}

int batteryPercent(float v) {
  static const float tbl[][2] = {{4.20, 100}, {4.10, 90}, {4.00, 80}, {3.90, 65}, {3.80, 50},
                                 {3.75, 40},  {3.70, 30}, {3.65, 20}, {3.60, 12}, {3.50, 5}, {3.30, 0}};
  if (v >= tbl[0][0]) return 100;
  for (unsigned i = 1; i < sizeof(tbl) / sizeof(tbl[0]); i++)
    if (v >= tbl[i][0])
      return tbl[i][1] + (v - tbl[i][0]) * (tbl[i - 1][1] - tbl[i][1]) / (tbl[i - 1][0] - tbl[i][0]);
  return 0;
}

// ------------------------------------------------------------------ button
enum BtnEvent { EV_NONE, EV_SHORT, EV_LONG };
bool bPrev = false, bLongDone = false;
uint32_t bT0 = 0;

void buttonInit() {                 // the wake-up press must not count as a click
  bPrev = btnDown();
  bLongDone = bPrev;
}

BtnEvent pollButton() {
  bool d = btnDown();
  uint32_t now = millis();
  BtnEvent e = EV_NONE;
  if (d && !bPrev) { bT0 = now; bLongDone = false; }
  if (d && !bLongDone && now - bT0 >= LONG_PRESS_MS) { e = EV_LONG; bLongDone = true; }
  if (!d && bPrev && !bLongDone && now - bT0 >= 25) e = EV_SHORT;
  bPrev = d;
  if (d || e != EV_NONE) lastActivity = now;
  return e;
}

// ------------------------------------------------------------------ drawing helpers
void drawBattery(int x, int y) {
  float v = readVbat();
  int p = batteryPercent(v);
  oled.drawFrame(x, y, 18, 9);
  oled.drawBox(x + 18, y + 2, 2, 5);
  oled.drawBox(x + 2, y + 2, (14 * p) / 100, 5);
  if (usbPresent()) {                // charging bolt
    oled.drawLine(x - 6, y, x - 9, y + 4);
    oled.drawLine(x - 9, y + 4, x - 5, y + 4);
    oled.drawLine(x - 5, y + 4, x - 8, y + 8);
  }
  oled.setFont(u8g2_font_5x7_tf);
  oled.setCursor(x - 28, y + 7);
  oled.print(p);
  oled.print('%');
}

void screenTemperature() {
  oled.clearBuffer();
  drawBattery(106, 0);
  if (isnan(tempC)) {
    oled.setFont(u8g2_font_7x14B_tf);
    oled.drawStr(4, 36, "SHT40 error");
  } else {
    char buf[12];
    snprintf(buf, sizeof(buf), "%.1f", tempC);
    oled.setFont(u8g2_font_logisoso28_tn);
    int w = oled.getStrWidth(buf);
    int x = (128 - w - 14) / 2;
    oled.drawStr(x, 44, buf);
    oled.drawCircle(x + w + 4, 18, 2);          // degree sign
    oled.setFont(u8g2_font_7x14B_tf);
    oled.drawStr(x + w + 8, 30, "C");
    oled.setFont(u8g2_font_6x10_tf);
    snprintf(buf, sizeof(buf), "RH %.0f %%", humid);
    oled.drawStr((128 - oled.getStrWidth(buf)) / 2, 60, buf);
  }
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(0, 7, "ThermoBox");
  oled.sendBuffer();
}

void screenStats() {
  char buf[24];
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 9, "Statistics");
  oled.drawHLine(0, 11, 128);
  if (rtcSamples) {
    snprintf(buf, sizeof(buf), "Min  %.1f C", rtcMin); oled.drawStr(0, 24, buf);
    snprintf(buf, sizeof(buf), "Max  %.1f C", rtcMax); oled.drawStr(0, 35, buf);
  }
  snprintf(buf, sizeof(buf), "Batt %.2f V", readVbat()); oled.drawStr(0, 46, buf);
  oled.drawStr(0, 58, usbPresent() ? "USB: charging" : "USB: -");
  oled.sendBuffer();
}

void centerText(const char *s, int y, const uint8_t *font) {
  oled.setFont(font);
  oled.drawStr((128 - oled.getStrWidth(s)) / 2, y, s);
}

// ------------------------------------------------------------------ games
// Add your own game: write   uint32_t myGame()   that returns a score,
// then add a line to GAMES[] below. Use btnDown() / pollButton() for input.

uint32_t gameCaveFlyer();
uint32_t gameJumpRunner();
uint32_t gameReaction();

struct Game { const char *name; uint32_t (*run)(); bool lowerIsBetter; };
Game GAMES[] = {
  {"Cave Flyer", gameCaveFlyer, false},
  {"Jump Runner", gameJumpRunner, false},
  {"Reaction", gameReaction, true},
};
constexpr int GAME_COUNT = sizeof(GAMES) / sizeof(GAMES[0]);

bool frameDue(uint32_t &t, uint32_t periodMs = 33) {   // ~30 fps
  uint32_t now = millis();
  if (now - t < periodMs) { delay(1); return false; }   // delay() feeds the idle task / watchdog
  t = now;
  return true;
}

// Hold the button to climb, release to fall. Fly through the gaps.
uint32_t gameCaveFlyer() {
  float y = 32, vy = 0;
  const int PX = 20;
  struct Col { float x; int gapY; } cols[3];
  int gap = 26;
  for (int i = 0; i < 3; i++) cols[i] = {128.0f + i * 48, (int)random(10, 64 - 10 - gap)};
  float speed = 1.4f;
  uint32_t score = 0, t = 0;
  while (true) {
    if (!frameDue(t)) continue;
    pollButton();
    vy += btnDown() ? -0.32f : 0.25f;
    vy = constrain(vy, -2.6f, 2.8f);
    y += vy;
    oled.clearBuffer();
    for (auto &c : cols) {
      c.x -= speed;
      if (c.x < -8) {
        c.x += 3 * 48;
        gap = max(16, 26 - (int)score / 4);
        c.gapY = random(4, 64 - 4 - gap);
        score++;
        speed = min(3.0f, 1.4f + score * 0.05f);
      }
      oled.drawBox((int)c.x, 0, 8, c.gapY);
      oled.drawBox((int)c.x, c.gapY + gap, 8, 64 - c.gapY - gap);
      bool inX = PX + 5 > c.x && PX < c.x + 8;
      if (inX && (y < c.gapY || y + 5 > c.gapY + gap)) return score;
    }
    if (y < 0 || y > 59) return score;
    oled.drawBox(PX, (int)y, 6, 5);
    oled.setFont(u8g2_font_5x7_tf);
    oled.setCursor(110, 7);
    oled.print(score);
    oled.sendBuffer();
  }
}

// Press to jump over the blocks. It gets faster.
uint32_t gameJumpRunner() {
  const int GROUND = 56, PX = 14;
  float y = GROUND - 8, vy = 0;
  float ox = 140, speed = 2.0f;
  int ow = 6, oh = 10;
  uint32_t score = 0, t = 0, frames = 0;
  while (true) {
    if (!frameDue(t)) continue;
    bool onGround = y >= GROUND - 8;
    if (btnDown() && onGround) vy = -4.2f;
    pollButton();
    vy += 0.3f;
    y = min(y + vy, (float)GROUND - 8);
    ox -= speed;
    if (ox < -ow) {
      ox = 128 + random(0, 60);
      ow = random(4, 10);
      oh = random(6, 16);
    }
    if (++frames % 6 == 0) score++;
    speed = min(5.0f, 2.0f + score / 60.0f);
    if (PX + 8 > ox && PX < ox + ow && y + 8 > GROUND - oh) return score;
    oled.clearBuffer();
    oled.drawHLine(0, GROUND, 128);
    for (int i = 0; i < 128; i += 9) oled.drawPixel((i + 128 - (frames * (int)speed) % 128) % 128, GROUND + 3);
    oled.drawBox(PX, (int)y, 8, 8);
    oled.drawBox((int)ox, GROUND - oh, ow, oh);
    oled.setFont(u8g2_font_5x7_tf);
    oled.setCursor(100, 7);
    oled.print(score);
    oled.sendBuffer();
  }
}

// Wait for GO!, press as fast as you can. Score = milliseconds (lower is better).
uint32_t gameReaction() {
  oled.clearBuffer();
  centerText("wait...", 38, u8g2_font_7x14B_tf);
  oled.sendBuffer();
  while (btnDown()) delay(1);
  uint32_t start = millis(), wait = random(1500, 4500);
  while (millis() - start < wait) {
    if (btnDown()) {
      oled.clearBuffer();
      centerText("Too soon!", 38, u8g2_font_7x14B_tf);
      oled.sendBuffer();
      while (btnDown()) delay(1);
      delay(800);
      return 9999;
    }
  }
  oled.clearBuffer();
  oled.drawBox(0, 0, 128, 64);
  oled.setDrawColor(0);
  centerText("GO!", 42, u8g2_font_logisoso28_tr);
  oled.setDrawColor(1);
  oled.sendBuffer();
  uint32_t t0 = millis();
  while (!btnDown()) delay(0);
  uint32_t ms = millis() - t0;
  while (btnDown()) delay(1);
  buttonInit();
  return ms;
}

void playGame(int idx) {
  Game &g = GAMES[idx];
  // countdown
  for (int i = 3; i > 0; i--) {
    oled.clearBuffer();
    centerText(g.name, 16, u8g2_font_7x14B_tf);
    char c[2] = {(char)('0' + i), 0};
    centerText(c, 52, u8g2_font_logisoso28_tn);
    oled.sendBuffer();
    delay(500);
  }
  buttonInit();
  uint32_t score = g.run();

  char key[8];
  snprintf(key, sizeof(key), "hi%d", idx);
  prefs.begin("thermobox", false);
  uint32_t best = prefs.getUInt(key, g.lowerIsBetter ? 99999 : 0);
  bool record = g.lowerIsBetter ? score < best : score > best;
  if (record) { best = score; prefs.putUInt(key, best); }
  prefs.end();

  char buf[24];
  oled.clearBuffer();
  centerText(record ? "NEW RECORD!" : "GAME OVER", 14, u8g2_font_7x14B_tf);
  snprintf(buf, sizeof(buf), g.lowerIsBetter ? "%lu ms" : "Score %lu", (unsigned long)score);
  centerText(buf, 36, u8g2_font_7x14B_tf);
  snprintf(buf, sizeof(buf), g.lowerIsBetter ? "Best %lu ms" : "Best %lu", (unsigned long)best);
  centerText(buf, 54, u8g2_font_6x10_tf);
  oled.sendBuffer();
  while (btnDown()) delay(1);
  delay(600);
  buttonInit();
  lastActivity = millis();
  while (millis() - lastActivity < 4000 && pollButton() == EV_NONE) delay(5);
  lastActivity = millis();
}

void gameMenu() {
  int sel = 0;
  bool redraw = true;
  lastActivity = millis();
  while (true) {
    if (redraw) {
      oled.clearBuffer();
      oled.setFont(u8g2_font_6x10_tf);
      oled.drawStr(0, 9, "Games");
      oled.drawStr(68, 9, "long=play");
      oled.drawHLine(0, 11, 128);
      for (int i = 0; i <= GAME_COUNT; i++) {
        const char *name = i < GAME_COUNT ? GAMES[i].name : "< Back";
        int y = 23 + i * 11;
        if (i == sel) { oled.drawBox(0, y - 9, 128, 11); oled.setDrawColor(0); }
        oled.drawStr(4, y, name);
        oled.setDrawColor(1);
      }
      oled.sendBuffer();
      redraw = false;
    }
    BtnEvent e = pollButton();
    if (e == EV_SHORT) { sel = (sel + 1) % (GAME_COUNT + 1); redraw = true; }
    if (e == EV_LONG) {
      if (sel == GAME_COUNT) { while (btnDown()) delay(1); buttonInit(); return; }
      while (btnDown()) delay(1);
      playGame(sel);
      redraw = true;
    }
    if (millis() - lastActivity > (usbPresent() ? IDLE_USB_MS : IDLE_MS) * 2) goToSleep();
    delay(5);
  }
}

// ------------------------------------------------------------------ main
void setup() {
  pinMode(PIN_BTN, INPUT);           // external 100k pull-up + 10nF debounce
  pinMode(PIN_VBUS, INPUT);
  analogSetPinAttenuation(PIN_BAT, ADC_11db);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  peripheralsOn();
  sampleTemperature();               // measure first: MCU is still cold

  if (cause == ESP_SLEEP_WAKEUP_TIMER) {   // silent log wake-up, ~30 ms
    peripheralsOff();
    esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_BTN, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_sleep_enable_timer_wakeup((uint64_t)LOG_INTERVAL_S * 1000000ULL);
    esp_deep_sleep_start();
  }

  Serial.begin(115200);
  oled.begin();
  oled.setContrast(160);
  randomSeed(esp_random());
  buttonInit();
  lastActivity = millis();
  screenTemperature();
}

void loop() {
  static int screen = 0;
  static uint32_t lastRead = millis();

  BtnEvent e = pollButton();
  if (e == EV_SHORT) {
    screen = (screen + 1) % 2;
    sampleTemperature();
    lastRead = millis();
  } else if (e == EV_LONG) {
    gameMenu();
    sampleTemperature();
    lastRead = millis();
  }

  if (millis() - lastRead > 2000) {  // live update while awake
    sampleTemperature();
    lastRead = millis();
  }

  if (screen == 0) screenTemperature(); else screenStats();

  if (millis() - lastActivity > (usbPresent() ? IDLE_USB_MS : IDLE_MS)) goToSleep();
  delay(30);
}
