/*
  SULTAN ULTIMATE - CLEAN / SAFE BUILD
  Based on the supplied 896-line project.
  Focus: reliable OLED UI, buttons, IR capture/replay/analyzer,
         NVS storage, passive Wi-Fi/BLE/NRF24/CC1101 diagnostics.

  Required libraries:
  Adafruit GFX, Adafruit SSD1306, IRremoteESP8266, RF24,
  ELECHOUSE CC1101 library, and the ESP32 Arduino core.
*/

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
#include <RF24.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>

#define OLED_W 128
#define OLED_H 64
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);

#define BTN_UP 12
#define BTN_DOWN 14
#define BTN_LEFT 25
#define BTN_RIGHT 26
#define BTN_SEL 27
#define BTN_BACK 32

#define IR_RX 15
#define IR_TX 4
#define NRF_CE 22
#define NRF_CSN 21
#define CC1101_CS 5

#define MAX_RAW_LEN 128
#define TOTAL_SLOTS 5
#define SUBGHZ_SLOTS 5
#define AUTO_SLEEP_TIMEOUT 45000UL
#define DEBOUNCE_MS 160UL

IRrecv irrecv(IR_RX, 1024, 50, true);
IRsend irsend(IR_TX);
decode_results results;
RF24 radio(NRF_CE, NRF_CSN);

Preferences prefs;
Preferences subPrefs;

std::vector<uint16_t> rawCodes[TOTAL_SLOTS];
String signalTypes[TOTAL_SLOTS];
bool slotActive[TOTAL_SLOTS] = {false};

std::vector<byte> subGhzCodes[SUBGHZ_SLOTS];
float subGhzFreqs[SUBGHZ_SLOTS] = {0};
bool subGhzSlotActive[SUBGHZ_SLOTS] = {false};

uint8_t nrfCapturedPayload[32] = {0};
bool nrfHasCaptured = false;
float currentFreq = 433.92f;

int menuIndex = 0;
unsigned long lastBtn = 0;
unsigned long lastActivity = 0;

const char *menuItems[] = {
  "1. CAPTURE IR", "2. SAVED IR", "3. IR TEST",
  "4. IR TEST 2", "5. IR RANDOM TEST", "6. IR FUZZ TEST",
  "7. IR INVERT TEST", "8. WEB STATUS", "9. IR ANALYZER",
  "10. IR TEST", "11. HEX SENDER", "12. IR RANGE TEST",
  "13. BLE STATUS", "14. WIFI SCAN", "15. WIFI STATUS",
  "16. NRF DIAGNOSTIC", "17. SUB-GHZ RX", "18. SAVED SUB-GHZ"
};
const int TOTAL_MENU_ITEMS = sizeof(menuItems) / sizeof(menuItems[0]);

bool btnPressed(int pin) {
  if (digitalRead(pin) == LOW && millis() - lastBtn >= DEBOUNCE_MS) {
    lastBtn = millis();
    lastActivity = millis();
    return true;
  }
  return false;
}

void waitForButton() {
  while (!btnPressed(BTN_SEL) && !btnPressed(BTN_BACK))
    vTaskDelay(10 / portTICK_PERIOD_MS);
}

void showHeader(const char *title) {
  display.clearDisplay();
  display.fillRect(0, 0, 128, 12, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(2, 2);
  display.print("ULT::");
  display.print(title);
  display.setCursor(98, 2);
  display.print(ESP.getFreeHeap() / 1024);
  display.print("K");
  display.setTextColor(SSD1306_WHITE);
  display.drawRoundRect(0, 13, 128, 51, 2, SSD1306_WHITE);
}

void messageScreen(const char *title, const char *line1, const char *line2 = "") {
  showHeader(title);
  display.setCursor(7, 28);
  display.print(line1);
  if (line2[0]) {
    display.setCursor(7, 42);
    display.print(line2);
  }
  display.display();
}

void progress(const char *label) {
  for (int i = 0; i <= 100; i += 25) {
    showHeader("SYS");
    display.setCursor(8, 22);
    display.print(label);
    display.drawRect(8, 38, 112, 8, SSD1306_WHITE);
    display.fillRect(10, 40, map(i, 0, 100, 0, 108), 4, SSD1306_WHITE);
    display.display();
    delay(5);
  }
}

void playBootAnimation() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(17, 25);
  display.print("SULTAN ULTIMATE");
  display.setCursor(25, 40);
  display.print("CLEAN BUILD");
  display.display();
  delay(500);
}

void checkPinLock() {
  const int pinSeq[] = {BTN_UP, BTN_DOWN, BTN_SEL};
  int step = 0, attempts = 0;

  while (step < 3) {
    showHeader("SEC");
    display.setCursor(8, 25);
    display.print("SECURE PIN");
    display.setCursor(8, 42);
    display.print("[ ");
    for (int i = 0; i < 3; i++) display.print(i < step ? "* " : "_ ");
    display.print("]");
    display.display();

    if (btnPressed(BTN_UP)) {
      if (pinSeq[step] == BTN_UP) step++;
      else { step = 0; attempts++; }
    } else if (btnPressed(BTN_DOWN)) {
      if (pinSeq[step] == BTN_DOWN) step++;
      else { step = 0; attempts++; }
    } else if (btnPressed(BTN_SEL)) {
      if (pinSeq[step] == BTN_SEL) step++;
      else { step = 0; attempts++; }
    }

    if (attempts >= 3) {
      messageScreen("LOCKED", "Wait 3s...");
      delay(3000);
      attempts = 0;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

bool saveToNVS(int slot) {
  if (slot < 0 || slot >= TOTAL_SLOTS || rawCodes[slot].empty()) return false;
  if (!prefs.begin("sult_ult", false)) return false;

  char key[12];
  snprintf(key, sizeof(key), "l_%d", slot);
  prefs.putUShort(key, (uint16_t)rawCodes[slot].size());

  snprintf(key, sizeof(key), "t_%d", slot);
  prefs.putString(key, signalTypes[slot]);

  snprintf(key, sizeof(key), "d_%d", slot);
  size_t bytes = rawCodes[slot].size() * sizeof(uint16_t);
  bool ok = prefs.putBytes(key, rawCodes[slot].data(), bytes) == bytes;
  prefs.end();
  return ok;
}

void loadFromNVS() {
  if (!prefs.begin("sult_ult", true)) return;

  for (int i = 0; i < TOTAL_SLOTS; i++) {
    char key[12];
    snprintf(key, sizeof(key), "l_%d", i);
    uint16_t len = prefs.getUShort(key, 0);

    if (len > 0 && len <= MAX_RAW_LEN) {
      snprintf(key, sizeof(key), "t_%d", i);
      signalTypes[i] = prefs.getString(key, "RAW");

      rawCodes[i].resize(len);
      snprintf(key, sizeof(key), "d_%d", i);
      size_t got = prefs.getBytes(key, rawCodes[i].data(),
                                  len * sizeof(uint16_t));
      if (got == len * sizeof(uint16_t)) slotActive[i] = true;
      else rawCodes[i].clear();
    }
  }
  prefs.end();
}

bool saveSubGhzToNVS(int slot) {
  if (slot < 0 || slot >= SUBGHZ_SLOTS || subGhzCodes[slot].empty())
    return false;
  if (!subPrefs.begin("sub_ult", false)) return false;

  char key[12];
  snprintf(key, sizeof(key), "sl_%d", slot);
  subPrefs.putUChar(key, (uint8_t)subGhzCodes[slot].size());

  snprintf(key, sizeof(key), "sf_%d", slot);
  subPrefs.putBytes(key, &subGhzFreqs[slot], sizeof(float));

  snprintf(key, sizeof(key), "sd_%d", slot);
  size_t n = subGhzCodes[slot].size();
  size_t got = subPrefs.putBytes(key, subGhzCodes[slot].data(), n);
  subPrefs.end();
  return got == n;
}

void loadSubGhzFromNVS() {
  if (!subPrefs.begin("sub_ult", true)) return;

  for (int i = 0; i < SUBGHZ_SLOTS; i++) {
    char key[12];
    snprintf(key, sizeof(key), "sl_%d", i);
    uint8_t len = subPrefs.getUChar(key, 0);

    if (len > 0 && len <= 64) {
      subGhzCodes[i].resize(len);

      snprintf(key, sizeof(key), "sf_%d", i);
      if (subPrefs.getBytes(key, &subGhzFreqs[i], sizeof(float)) != sizeof(float)) {
        subGhzCodes[i].clear();
        continue;
      }

      snprintf(key, sizeof(key), "sd_%d", i);
      if (subPrefs.getBytes(key, subGhzCodes[i].data(), len) == len)
        subGhzSlotActive[i] = true;
      else
        subGhzCodes[i].clear();
    }
  }
  subPrefs.end();
}

int firstFreeIRSlot() {
  for (int i = 0; i < TOTAL_SLOTS; i++)
    if (!slotActive[i]) return i;
  return -1;
}

int firstFreeSubSlot() {
  for (int i = 0; i < SUBGHZ_SLOTS; i++)
    if (!subGhzSlotActive[i]) return i;
  return -1;
}

void captureIR() {
  irrecv.enableIRIn();
  unsigned long start = millis();
  bool got = false;

  while (millis() - start < 8000) {
    showHeader("IR CAPTURE");
    display.setCursor(8, 22);
    display.print("Aim Remote...");
    display.setCursor(8, 40);
    display.print("Time: ");
    display.print(8 - (millis() - start) / 1000);
    display.print("s");
    display.display();

    if (irrecv.decode(&results)) {
      if (results.rawlen > 4 && results.rawlen <= MAX_RAW_LEN) {
        got = true;
        break;
      }
      irrecv.resume();
    }

    if (btnPressed(BTN_BACK)) {
      irrecv.disableIRIn();
      return;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }

  if (!got) {
    irrecv.disableIRIn();
    messageScreen("TIMEOUT", "No Signal!");
    display.display();
    waitForButton();
    return;
  }

  int slot = firstFreeIRSlot();
  if (slot < 0) {
    irrecv.disableIRIn();
    messageScreen("FULL", "IR Memory Full!");
    display.display();
    waitForButton();
    return;
  }

  rawCodes[slot].clear();
  for (uint16_t i = 1; i < results.rawlen &&
       rawCodes[slot].size() < MAX_RAW_LEN; i++) {
    rawCodes[slot].push_back(results.rawbuf[i] * RAWTICK);
  }

  signalTypes[slot] =
      (results.decode_type == UNKNOWN) ? "RAW" : typeToString(results.decode_type);

  slotActive[slot] = !rawCodes[slot].empty();
  bool saved = slotActive[slot] && saveToNVS(slot);

  irrecv.resume();
  irrecv.disableIRIn();

  messageScreen(saved ? "SAVED" : "ERROR",
                saved ? "Slot saved:" : "Save failed",
                saved ? String(slot + 1).c_str() : "");
  display.display();
  waitForButton();
}

void viewSaved() {
  int slot = 0;

  while (true) {
    bool any = false;
    for (int i = 0; i < TOTAL_SLOTS; i++)
      if (slotActive[i] && !rawCodes[i].empty()) any = true;

    if (!any) {
      messageScreen("SAVED IR", "Empty Storage!");
      display.display();
      waitForButton();
      return;
    }

    int guard = 0;
    while ((!slotActive[slot] || rawCodes[slot].empty()) &&
           guard++ < TOTAL_SLOTS)
      slot = (slot + 1) % TOTAL_SLOTS;

    showHeader("IR VIEWER");
    display.setCursor(7, 20);
    display.print("Slot "); display.print(slot + 1);
    display.print(": "); display.print(signalTypes[slot]);
    display.setCursor(7, 33);
    display.print("Pulses: "); display.print(rawCodes[slot].size());
    display.setCursor(7, 47);
    display.print("SEL TX / BACK");

    display.display();

    if (btnPressed(BTN_DOWN)) slot = (slot + 1) % TOTAL_SLOTS;
    if (btnPressed(BTN_UP)) slot = (slot - 1 + TOTAL_SLOTS) % TOTAL_SLOTS;
    if (btnPressed(BTN_BACK)) return;

    if (btnPressed(BTN_SEL)) {
      irsend.sendRaw(rawCodes[slot].data(),
                     rawCodes[slot].size(), 38);
      progress("Replaying...");
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void irAnalyzer() {
  irrecv.enableIRIn();
  unsigned long start = millis();

  while (millis() - start < 15000) {
    showHeader("ANALYZER");
    display.setCursor(7, 25);
    display.print("Listening...");
    display.setCursor(7, 42);
    display.print("BACK/SEL exit");
    display.display();

    if (irrecv.decode(&results)) {
      showHeader("ANALYZE");
      display.setCursor(4, 18);
      display.print("P:");
      display.print(typeToString(results.decode_type));
      display.setCursor(4, 32);
      display.print("H:0x");
      display.print(results.value, HEX);
      display.setCursor(4, 46);
      display.print("Raw:");
      display.print(results.rawlen);
      display.display();
      irrecv.resume();
      delay(400);
    }

    if (btnPressed(BTN_BACK) || btnPressed(BTN_SEL)) break;
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  irrecv.disableIRIn();
}

void irTest(const char *name, uint32_t code) {
  showHeader(name);
  display.setCursor(7, 27);
  display.print("Sending test...");
  display.display();
  irsend.sendNEC(code, 32);
  delay(100);
  messageScreen("DONE", "IR test sent");
  display.display();
  waitForButton();
}

void randomIRTest() {
  uint32_t code = esp_random();
  irTest("IR RANDOM", code);
}

void fuzzIRTest() {
  if (!slotActive[0] || rawCodes[0].empty()) {
    messageScreen("FUZZ TEST", "Slot 1 Empty!");
    display.display();
    waitForButton();
    return;
  }
  // Local/offline timing experiment only; no repeated blasting.
  std::vector<uint16_t> b = rawCodes[0];
  for (size_t i = 0; i < b.size(); ++i) {
    int v = (int)b[i] + random(-20, 21);
    b[i] = (uint16_t)max(1, v);
  }
  irsend.sendRaw(b.data(), b.size(), 38);
  messageScreen("FUZZ TEST", "One test sent");
  display.display();
  waitForButton();
}

void invertIRTest() {
  if (!slotActive[0] || rawCodes[0].empty()) {
    messageScreen("INVERT TEST", "Slot 1 Empty!");
    display.display();
    waitForButton();
    return;
  }
  std::vector<uint16_t> b = rawCodes[0];
  for (size_t i = 0; i < b.size(); ++i) {
    int v = (i & 1) ? (int)b[i] - 20 : (int)b[i] + 20;
    b[i] = (uint16_t)max(1, v);
  }
  irsend.sendRaw(b.data(), b.size(), 38);
  messageScreen("INVERT TEST", "One test sent");
  display.display();
  waitForButton();
}

void customHexSender() {
  static uint32_t hexVal = 0x20DF10EF;
  int selectedDigit = 0;
  const int numDigits = 8;

  while (true) {
    showHeader("HEX SENDER");
    display.setCursor(6, 18);
    display.print("0x");
    char s[12];
    snprintf(s, sizeof(s), "%08lX", (unsigned long)hexVal);
    display.print(s);
    display.setCursor(6, 32);
    display.print("UP/DN edit L/R move");
    display.setCursor(6, 47);
    display.print("SEL send BACK exit");
    display.setCursor(6 + selectedDigit * 6, 57);
    display.print("^");
    display.display();

    if (btnPressed(BTN_UP)) {
      uint32_t sh = (numDigits - 1 - selectedDigit) * 4;
      uint32_t d = (hexVal >> sh) & 0xF;
      d = (d + 1) & 0xF;
      hexVal = (hexVal & ~(0xFUL << sh)) | (d << sh);
    }
    if (btnPressed(BTN_DOWN)) {
      uint32_t sh = (numDigits - 1 - selectedDigit) * 4;
      uint32_t d = (hexVal >> sh) & 0xF;
      d = (d + 15) & 0xF;
      hexVal = (hexVal & ~(0xFUL << sh)) | (d << sh);
    }
    if (btnPressed(BTN_RIGHT)) selectedDigit = (selectedDigit + 1) % numDigits;
    if (btnPressed(BTN_LEFT)) selectedDigit = (selectedDigit + numDigits - 1) % numDigits;
    if (btnPressed(BTN_BACK)) return;
    if (btnPressed(BTN_SEL)) {
      irsend.sendNEC(hexVal, 32);
      messageScreen("HEX", "One code sent");
      display.display();
      delay(250);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void webStatus() {
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP("SULTAN_STATUS", "12345678")) {
    messageScreen("WEB", "AP start failed");
    display.display();
    waitForButton();
    WiFi.mode(WIFI_OFF);
    return;
  }

  IPAddress ip = WiFi.softAPIP();
  WiFiServer web(80);
  web.begin();

  while (!btnPressed(BTN_BACK)) {
    WiFiClient c = web.available();
    if (c) {
      unsigned long t = millis();
      while (c.connected() && !c.available() && millis() - t < 1000) delay(1);
      while (c.available()) c.read();

      String html = "<!doctype html><html><body><h2>SULTAN STATUS</h2>";
      html += "<p>Free heap: " + String(ESP.getFreeHeap()) + "</p>";
      html += "<p>IR slots: ";
      for (int i = 0; i < TOTAL_SLOTS; i++)
        html += slotActive[i] ? "USED " : "EMPTY ";
      html += "</p></body></html>";

      c.println("HTTP/1.1 200 OK");
      c.println("Content-Type: text/html");
      c.println("Connection: close");
      c.println();
      c.print(html);
      c.stop();
    }

    showHeader("WEB STATUS");
    display.setCursor(4, 22); display.print("SSID: SULTAN_STATUS");
    display.setCursor(4, 35); display.print("IP: "); display.print(ip);
    display.setCursor(4, 48); display.print("BACK: Close");
    display.display();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }

  web.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
}

void wifiScan() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(50);

  int n = WiFi.scanNetworks(false, true);
  showHeader("WIFI SCAN");
  if (n <= 0) {
    display.setCursor(7, 30);
    display.print("No AP found");
    display.display();
    waitForButton();
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
    return;
  }

  int selected = 0;
  while (true) {
    showHeader("WIFI SCAN");
    int start = selected > 2 ? selected - 2 : 0;
    for (int i = 0; i < 3 && start + i < n; i++) {
      int idx = start + i;
      display.setCursor(5, 20 + i * 13);
      display.print(idx == selected ? ">" : " ");
      String ssid = WiFi.SSID(idx);
      if (ssid.length() > 15) ssid = ssid.substring(0, 15);
      display.print(ssid);
    }
    display.display();

    if (btnPressed(BTN_UP)) selected = (selected - 1 + n) % n;
    if (btnPressed(BTN_DOWN)) selected = (selected + 1) % n;
    if (btnPressed(BTN_BACK)) break;
    if (btnPressed(BTN_SEL)) {
      showHeader("AP INFO");
      display.setCursor(5, 20);
      display.print(WiFi.SSID(selected));
      display.setCursor(5, 33);
      display.print("CH: "); display.print(WiFi.channel(selected));
      display.setCursor(5, 46);
      display.print("RSSI: "); display.print(WiFi.RSSI(selected));
      display.display();
      waitForButton();
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }

  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);
}

void wifiStatus() {
  WiFi.mode(WIFI_STA);
  showHeader("WIFI STATUS");
  display.setCursor(6, 23); display.print("STA ready");
  display.setCursor(6, 37); display.print("MAC:");
  display.setCursor(6, 49); display.print(WiFi.macAddress());
  display.display();
  waitForButton();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void bleStatus() {
  // Passive status screen; no advertising/spam.
  showHeader("BLE STATUS");
  display.setCursor(6, 25);
  display.print("ESP32 BLE ready");
  display.setCursor(6, 40);
  display.print("No advertising");
  display.display();
  waitForButton();
}

void nrfDiagnostic() {
  if (!radio.begin()) {
    messageScreen("NRF ERROR", "Module not found");
    display.display();
    waitForButton();
    return;
  }

  radio.stopListening();
  radio.setAutoAck(false);
  radio.setRetries(0, 0);
  radio.setDataRate(RF24_1MBPS);
  radio.setPALevel(RF24_PA_LOW);

  while (true) {
    showHeader("NRF DIAG");
    display.setCursor(6, 22); display.print("Module: OK");
    display.setCursor(6, 35); display.print("CE/CSN: ");
    display.print(NRF_CE); display.print("/"); display.print(NRF_CSN);
    display.setCursor(6, 48); display.print("SEL test / BACK");
    display.display();

    if (btnPressed(BTN_SEL)) {
      showHeader("NRF TEST");
      display.setCursor(6, 30);
      display.print(radio.isChipConnected() ? "Chip connected" : "No chip");
      display.display();
      waitForButton();
    }
    if (btnPressed(BTN_BACK)) return;
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void subGhzRX() {
  ELECHOUSE_cc1101.setSpiPin(18, 19, 23, CC1101_CS);
  if (!ELECHOUSE_cc1101.init()) {
    messageScreen("CC1101 ERROR", "Module not found");
    display.display();
    waitForButton();
    return;
  }

  ELECHOUSE_cc1101.setMHZ(currentFreq);

  while (true) {
    showHeader("SUB-GHZ RX");
    display.setCursor(6, 20); display.print("Freq: "); display.print(currentFreq);
    display.setCursor(6, 34); display.print("UP: receive");
    display.setCursor(6, 47); display.print("L/R freq BACK");
    display.display();

    if (btnPressed(BTN_LEFT)) {
      currentFreq = currentFreq == 433.92f ? 315.00f : 433.92f;
      ELECHOUSE_cc1101.setMHZ(currentFreq);
    }
    if (btnPressed(BTN_RIGHT)) {
      currentFreq = currentFreq == 315.00f ? 433.92f : 315.00f;
      ELECHOUSE_cc1101.setMHZ(currentFreq);
    }
    if (btnPressed(BTN_BACK)) return;

    if (btnPressed(BTN_UP)) {
      ELECHOUSE_cc1101.SetReceive();
      unsigned long start = millis();
      byte buf[64];
      byte len = 0;
      bool captured = false;

      while (millis() - start < 10000) {
        if (ELECHOUSE_cc1101.CheckRxFifo(100)) {
          len = ELECHOUSE_cc1101.ReceiveData(buf);
          if (len > 0 && len <= 64) {
            int slot = firstFreeSubSlot();
            if (slot >= 0) {
              subGhzCodes[slot].assign(buf, buf + len);
              subGhzFreqs[slot] = currentFreq;
              subGhzSlotActive[slot] = saveSubGhzToNVS(slot);
              captured = subGhzSlotActive[slot];
            }
            break;
          }
        }
        if (btnPressed(BTN_BACK)) break;
        vTaskDelay(10 / portTICK_PERIOD_MS);
      }

      messageScreen("RESULT", captured ? "Saved" : "No signal");
      display.display();
      waitForButton();
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void viewSavedSubGhz() {
  int slot = 0;

  while (true) {
    bool any = false;
    for (int i = 0; i < SUBGHZ_SLOTS; i++)
      if (subGhzSlotActive[i] && !subGhzCodes[i].empty()) any = true;

    if (!any) {
      messageScreen("SUB SAVED", "Empty Storage!");
      display.display();
      waitForButton();
      return;
    }

    int guard = 0;
    while ((!subGhzSlotActive[slot] || subGhzCodes[slot].empty()) &&
           guard++ < SUBGHZ_SLOTS)
      slot = (slot + 1) % SUBGHZ_SLOTS;

    showHeader("SUB SAVED");
    display.setCursor(6, 20); display.print("Slot "); display.print(slot + 1);
    display.setCursor(6, 33); display.print("Freq: "); display.print(subGhzFreqs[slot]);
    display.print("MHz");
    display.setCursor(6, 46); display.print("Bytes: "); display.print(subGhzCodes[slot].size());
    display.display();

    if (btnPressed(BTN_UP)) slot = (slot + SUBGHZ_SLOTS - 1) % SUBGHZ_SLOTS;
    if (btnPressed(BTN_DOWN)) slot = (slot + 1) % SUBGHZ_SLOTS;
    if (btnPressed(BTN_BACK)) return;
    if (btnPressed(BTN_SEL)) {
      messageScreen("SUB SAVED", "Stored data only",
                    "TX not enabled in safe build");
      display.display();
      waitForButton();
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void setup() {
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  pinMode(BTN_SEL, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);

  Serial.begin(115200);
  Wire.begin();

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for (;;) delay(1000);
  }

  randomSeed((uint32_t)esp_random());
  playBootAnimation();
  checkPinLock();

  irsend.begin();
  loadFromNVS();
  loadSubGhzFromNVS();
  lastActivity = millis();
}

void drawMenu() {
  showHeader("ULTIMATE");
  int start = menuIndex >= 3 ? menuIndex - 2 : 0;

  for (int i = 0; i < 3; i++) {
    int idx = start + i;
    if (idx >= TOTAL_MENU_ITEMS) break;
    int y = 20 + i * 13;

    if (idx == menuIndex) {
      display.fillRect(4, y - 1, 120, 12, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(7, y);
    display.print(idx == menuIndex ? ">" : " ");
    display.print(menuItems[idx]);
  }
  display.setTextColor(SSD1306_WHITE);
  display.display();
}

void runMenuItem() {
  switch (menuIndex) {
    case 0: captureIR(); break;
    case 1: viewSaved(); break;
    case 2: irTest("IR TEST", 0x20DF10EF); break;
    case 3: irTest("IR TEST 2", 0xE0E040BF); break;
    case 4: randomIRTest(); break;
    case 5: fuzzIRTest(); break;
    case 6: invertIRTest(); break;
    case 7: webStatus(); break;
    case 8: irAnalyzer(); break;
    case 9: irTest("IR TEST", 0x61A0F00F); break;
    case 10: customHexSender(); break;
    case 11: irTest("IR RANGE", 0x4004); break;
    case 12: bleStatus(); break;
    case 13: wifiScan(); break;
    case 14: wifiStatus(); break;
    case 15: nrfDiagnostic(); break;
    case 16: subGhzRX(); break;
    case 17: viewSavedSubGhz(); break;
  }
}

void loop() {
  if (millis() - lastActivity > AUTO_SLEEP_TIMEOUT) {
    display.clearDisplay();
    display.display();
    while (digitalRead(BTN_UP) == HIGH &&
           digitalRead(BTN_DOWN) == HIGH &&
           digitalRead(BTN_SEL) == HIGH &&
           digitalRead(BTN_BACK) == HIGH) {
      vTaskDelay(50 / portTICK_PERIOD_MS);
    }
    lastActivity = millis();
  }

  drawMenu();

  if (btnPressed(BTN_UP)) {
    menuIndex = (menuIndex - 1 + TOTAL_MENU_ITEMS) % TOTAL_MENU_ITEMS;
    return;
  }
  if (btnPressed(BTN_DOWN)) {
    menuIndex = (menuIndex + 1) % TOTAL_MENU_ITEMS;
    return;
  }
  if (btnPressed(BTN_SEL)) {
    runMenuItem();
    lastActivity = millis();
  }
  if (btnPressed(BTN_BACK)) {
    menuIndex = 0;
  }

  vTaskDelay(10 / portTICK_PERIOD_MS);
}
